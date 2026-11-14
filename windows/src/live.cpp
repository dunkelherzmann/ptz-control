// The camera's live video, decoded with Media Foundation's own H.264 and H.265 decoders (part of Windows; H.265
// comes with HEVC Video Extensions). Frames come from the core on the stream's own thread in Annex B form; the newest
// decoded picture is handed to the UI thread, skipping any the screen can't keep up with.
#include "app.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <cstdarg>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace {
void dbg(const char *fmt, ...) {
    static FILE *f = getenv("PTZ_LIVE_DEBUG") ? fopen(getenv("PTZ_LIVE_DEBUG"), "w") : nullptr;
    if (!f) return;
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fflush(f);
}

std::once_flag g_media_once;
HRESULT g_media_status = E_FAIL;

// Once per process: a multithreaded apartment that the stream threads join implicitly, and Media Foundation.
HRESULT start_media_foundation() {
    std::call_once(g_media_once, [] {
        CO_MTA_USAGE_COOKIE cookie;
        CoIncrementMTAUsage(&cookie);
        g_media_status = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    });
    return g_media_status;
}

std::wstring hex(HRESULT hr) {
    wchar_t text[16];
    swprintf(text, 16, L"0x%08lX", static_cast<unsigned long>(hr));
    return text;
}

// YUV to RGB, in 8.8 fixed point: R = k*(Y-y0) + rv*V, G = k*(Y-y0) - gu*U - gv*V, B = k*(Y-y0) + bu*U.
struct Matrix {
    int k, y0, rv, gu, gv, bu;
};
constexpr Matrix kBt601Limited{298, 16, 409, 100, 208, 516}, kBt709Limited{298, 16, 459, 55, 136, 541},
    kBt601Full{256, 0, 359, 88, 183, 454}, kBt709Full{256, 0, 403, 48, 120, 475};

unsigned char clamp8(int v) { return static_cast<unsigned char>(v < 0 ? 0 : v > 255 ? 255 : v); }

}  // namespace

class LiveDecoder : public std::enable_shared_from_this<LiveDecoder> {
public:
    LiveDecoder(std::function<void(std::function<void()>)> post, std::function<void(std::shared_ptr<LiveFrame>)> picture,
                std::function<void(const std::wstring &)> failed)
        : post_(std::move(post)), picture_(std::move(picture)), failed_(std::move(failed)) {}

    void decode(const ptz_frame &frame) {
        if (failed_once_ || !frame.data || !frame.size) return;
        if (!decoder_ || frame.codec != codec_) {
            if (!frame.keyframe) return;
            if (!open(frame.codec)) return;
        }
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        BYTE *bytes = nullptr;
        if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(frame.size), &buffer)) ||
            FAILED(buffer->Lock(&bytes, nullptr, nullptr)))
            return;
        std::memcpy(bytes, frame.data, frame.size);
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(frame.size));
        sample->AddBuffer(buffer.Get());
        sample->SetSampleTime(static_cast<LONGLONG>(frame.time * 1e7));
        if (frame.keyframe) sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);

        dbg("input %zu key %d", frame.size, frame.keyframe);
        HRESULT hr = decoder_->ProcessInput(0, sample.Get(), 0);
        dbg("input hr %lx", hr);
        if (hr == MF_E_NOTACCEPTING) {
            hr = drain();
            if (SUCCEEDED(hr)) hr = decoder_->ProcessInput(0, sample.Get(), 0);
        }
        if (SUCCEEDED(hr)) hr = drain();
        if (failed_once_) return;
        if (FAILED(hr)) {
            if (++failures_ >= 30) fail(L"Windows can't decode the camera's " + codec_name() + L" video (Media Foundation error " + hex(hr) + L")");
        } else {
            failures_ = 0;
        }
    }

    // After the stream has stopped: no more frames come.
    void close() { decoder_.Reset(); }

private:
    std::wstring codec_name() const { return codec_ == PTZ_CODEC_H265 ? L"H.265" : L"H.264"; }

    bool open(ptz_codec codec) {
        decoder_.Reset();
        have_output_ = streaming_ = false;
        codec_ = codec;
        HRESULT hr = start_media_foundation();
        if (FAILED(hr)) {
            fail(L"Media Foundation isn't available (error " + hex(hr) + L")");
            return false;
        }
        MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, codec == PTZ_CODEC_H265 ? MFVideoFormat_HEVC : MFVideoFormat_H264};
        IMFActivate **found = nullptr;
        UINT32 count = 0;
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                       &input, nullptr, &found, &count);
        if (SUCCEEDED(hr)) {
            for (UINT32 i = 0; i < count; ++i) {
                if (!decoder_ && SUCCEEDED(found[i]->ActivateObject(IID_PPV_ARGS(&decoder_))) && !set_input(input.guidSubtype))
                    decoder_.Reset();
                found[i]->Release();
            }
            CoTaskMemFree(found);
        }
        if (!decoder_) {
            fail(codec == PTZ_CODEC_H265
                     ? L"Windows has no H.265 decoder; install HEVC Video Extensions from the Microsoft Store, or choose a "
                       L"camera profile with H.264 video"
                     : L"Windows has no H.264 decoder; on Windows N, install the Media Feature Pack");
            return false;
        }
        // The decoder may only know its output once it has seen the first frame; some (H.265) need a picture size
        // first, and take a stand-in until the camera's video settings change it.
        have_output_ = choose_output() || (set_input(input.guidSubtype, 1920, 1080) && choose_output());
        dbg("open codec %d have_output %d flags %lx cb %lu", codec, have_output_, output_info_.dwFlags, output_info_.cbSize);
        if (have_output_) begin_streaming();
        return true;
    }

    // Some decoders (H.265) fail when told before they have an output type.
    void begin_streaming() {
        if (streaming_) return;
        streaming_ = true;
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        decoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }

    bool set_input(const GUID &subtype, UINT32 width = 0, UINT32 height = 0) {
        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(decoder_->GetAttributes(&attributes)) && attributes) attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        ComPtr<IMFMediaType> type;
        if (FAILED(MFCreateMediaType(&type))) return false;
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, subtype);
        type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (width && height) MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height);
        return SUCCEEDED(decoder_->SetInputType(0, type.Get(), 0));
    }

    // NV12 output, and the picture's size, visible area and colours.
    bool choose_output() {
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> type;
            if (FAILED(decoder_->GetOutputAvailableType(0, i, &type))) return false;
            GUID subtype{};
            if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_NV12) continue;
            if (FAILED(decoder_->SetOutputType(0, type.Get(), 0))) return false;
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
            if (!w || !h) return false;
            width_ = w;
            height_ = h;
            MFVideoArea area{};
            if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8 *>(&area), sizeof area, nullptr)) &&
                area.Area.cx > 0 && area.Area.cy > 0) {
                crop_x_ = static_cast<UINT32>(std::max<LONG>(0, area.OffsetX.value)) & ~1u;
                crop_y_ = static_cast<UINT32>(std::max<LONG>(0, area.OffsetY.value)) & ~1u;
                crop_w_ = std::min<UINT32>(static_cast<UINT32>(area.Area.cx), w - crop_x_);
                crop_h_ = std::min<UINT32>(static_cast<UINT32>(area.Area.cy), h - crop_y_);
            } else {
                crop_x_ = crop_y_ = 0;
                crop_w_ = w;
                crop_h_ = h;
            }
            stride_ = static_cast<LONG>(MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, w));
            if (stride_ < static_cast<LONG>(w)) stride_ = static_cast<LONG>(w);
            UINT32 range = MFGetAttributeUINT32(type.Get(), MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
            UINT32 matrix = MFGetAttributeUINT32(type.Get(), MF_MT_YUV_MATRIX,
                                                 crop_h_ >= 720 ? MFVideoTransferMatrix_BT709 : MFVideoTransferMatrix_BT601);
            bool hd = matrix == MFVideoTransferMatrix_BT709;
            matrix_ = range == MFNominalRange_0_255 ? (hd ? kBt709Full : kBt601Full) : (hd ? kBt709Limited : kBt601Limited);
            decoder_->GetOutputStreamInfo(0, &output_info_);
            dbg("output type %ux%u crop %u,%u %ux%u stride %ld flags %lx cb %lu", w, h, crop_x_, crop_y_, crop_w_, crop_h_, stride_, output_info_.dwFlags, output_info_.cbSize);
            return true;
        }
    }

    // Takes every picture the decoder has ready.
    HRESULT drain() {
        while (true) {
            MFT_OUTPUT_DATA_BUFFER output{};
            ComPtr<IMFSample> ours;
            bool provides = (output_info_.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            if (have_output_ && !provides) {
                ComPtr<IMFMediaBuffer> buffer;
                DWORD size = std::max<DWORD>(output_info_.cbSize, width_ * height_ * 3 / 2);
                if (FAILED(MFCreateSample(&ours)) || FAILED(MFCreateMemoryBuffer(size, &buffer))) return E_OUTOFMEMORY;
                ours->AddBuffer(buffer.Get());
                output.pSample = ours.Get();
            }
            DWORD status = 0;
            dbg("output have %d provides %d sample %p", have_output_, provides, output.pSample);
            HRESULT hr = decoder_->ProcessOutput(0, 1, &output, &status);
            dbg("output hr %lx", hr);
            if (output.pEvents) output.pEvents->Release();
            ComPtr<IMFSample> theirs;
            if (output.pSample && output.pSample != ours.Get()) theirs.Attach(output.pSample);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE || hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
                // The decoder has read the camera's video settings: take its new picture size.
                have_output_ = choose_output();
                if (!have_output_) return hr;
                begin_streaming();
                continue;
            }
            if (FAILED(hr)) return hr;
            if (IMFSample *sample = theirs ? theirs.Get() : ours.Get()) convert(sample);
        }
    }

    void convert(IMFSample *sample) {
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->GetBufferByIndex(0, &buffer))) return;
        ComPtr<IMF2DBuffer> planes;
        BYTE *base = nullptr;
        LONG pitch = 0;
        DWORD length = 0;
        bool two_d = SUCCEEDED(buffer.As(&planes)) && SUCCEEDED(planes->Lock2D(&base, &pitch));
        if (!two_d) {
            planes.Reset();
            if (FAILED(buffer->Lock(&base, nullptr, &length))) return;
            pitch = stride_;
            if (length < static_cast<DWORD>(pitch) * height_ * 3 / 2) {
                buffer->Unlock();
                return;
            }
        }
        // The chroma plane follows the luma plane, whose rows may be padded beyond the frame's height.
        UINT32 rows = height_;
        DWORD whole = 0;
        if (two_d && pitch > 0 && SUCCEEDED(planes->GetContiguousLength(&whole)) && whole / static_cast<DWORD>(pitch) * 2 / 3 > rows)
            rows = whole / static_cast<DWORD>(pitch) * 2 / 3;
        if (pitch > 0) {
            auto frame = std::make_shared<LiveFrame>();
            frame->width = static_cast<int>(crop_w_);
            frame->height = static_cast<int>(crop_h_);
            frame->bgra.resize(static_cast<size_t>(crop_w_) * crop_h_ * 4);
            const BYTE *chroma = base + static_cast<size_t>(pitch) * rows;
            const Matrix m = matrix_;
            for (UINT32 y = 0; y < crop_h_; ++y) {
                const BYTE *luma = base + static_cast<size_t>(pitch) * (crop_y_ + y) + crop_x_;
                const BYTE *uv = chroma + static_cast<size_t>(pitch) * ((crop_y_ + y) / 2) + crop_x_;
                unsigned char *out = frame->bgra.data() + static_cast<size_t>(y) * crop_w_ * 4;
                for (UINT32 x = 0; x < crop_w_; ++x) {
                    int c = m.k * (luma[x] - m.y0), d = uv[x & ~1u] - 128, e = uv[(x & ~1u) + 1] - 128;
                    out[0] = clamp8((c + m.bu * d + 128) >> 8);
                    out[1] = clamp8((c - m.gu * d - m.gv * e + 128) >> 8);
                    out[2] = clamp8((c + m.rv * e + 128) >> 8);
                    out[3] = 255;
                    out += 4;
                }
            }
            deliver(std::move(frame));
        }
        if (two_d) planes->Unlock2D();
        else buffer->Unlock();
    }

    // Hands the newest frame to the UI thread; one call waits there at a time, so frames it can't keep up with are
    // skipped.
    void deliver(std::shared_ptr<LiveFrame> frame) {
        bool schedule;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            newest_ = std::move(frame);
            schedule = !delivering_;
            delivering_ = true;
        }
        if (!schedule) return;
        auto self = shared_from_this();
        post_([self] {
            std::shared_ptr<LiveFrame> newest;
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                newest = std::move(self->newest_);
                self->delivering_ = false;
            }
            if (newest) self->picture_(newest);
        });
    }

    void fail(const std::wstring &reason) {
        failed_once_ = true;
        auto failed = failed_;
        post_([failed, reason] { failed(reason); });
    }

    std::function<void(std::function<void()>)> post_;
    std::function<void(std::shared_ptr<LiveFrame>)> picture_;
    std::function<void(const std::wstring &)> failed_;

    // The stream's thread only.
    ComPtr<IMFTransform> decoder_;
    ptz_codec codec_ = PTZ_CODEC_H264;
    MFT_OUTPUT_STREAM_INFO output_info_{};
    bool have_output_ = false, streaming_ = false, failed_once_ = false;
    int failures_ = 0;
    UINT32 width_ = 0, height_ = 0, crop_x_ = 0, crop_y_ = 0, crop_w_ = 0, crop_h_ = 0;
    LONG stride_ = 0;
    Matrix matrix_ = kBt601Limited;

    std::mutex mutex_;
    std::shared_ptr<LiveFrame> newest_;
    bool delivering_ = false;
};

std::shared_ptr<LiveDecoder> live_decoder_create(std::function<void(std::function<void()>)> post,
                                                 std::function<void(std::shared_ptr<LiveFrame>)> picture,
                                                 std::function<void(const std::wstring &)> failed) {
    return std::make_shared<LiveDecoder>(std::move(post), std::move(picture), std::move(failed));
}

void live_decoder_decode(LiveDecoder &decoder, const ptz_frame &frame) { decoder.decode(frame); }
void live_decoder_close(LiveDecoder &decoder) { decoder.close(); }
