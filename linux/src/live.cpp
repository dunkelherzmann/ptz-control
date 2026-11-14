// The camera's live video, decoded with the Flatpak runtime's GStreamer. Frames come from the core on the stream's
// own thread in Annex B form and go into appsrc ! parse ! decoder ! videoconvert ! appsink; the newest decoded
// picture is handed to the UI thread, skipping any the screen can't keep up with.
#include "app.h"

#include <QImage>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <memory>
#include <mutex>

namespace {

// The newest decoded picture, waiting for the UI thread.
struct Mailbox {
    std::mutex mutex;
    QImage newest;
    bool delivering = false;
};

bool gstreamer_ready(QString *error) {
    static bool ready = false;
    static QString failure;
    static std::once_flag once;
    std::call_once(once, [] {
        GError *e = nullptr;
        ready = gst_init_check(nullptr, nullptr, &e);
        if (!ready) failure = "GStreamer can't start" + (e ? QString(": ") + QString::fromUtf8(e->message) : QString());
        if (e) g_error_free(e);
    });
    if (!ready && error) *error = failure;
    return ready;
}

// Whether any installed decoder takes this kind of video.
bool has_decoder(GstCaps *caps) {
    GList *decoders = gst_element_factory_list_get_elements(
        GST_ELEMENT_FACTORY_TYPE_DECODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO, GST_RANK_MARGINAL);
    GList *usable = gst_element_factory_list_filter(decoders, caps, GST_PAD_SINK, FALSE);
    bool found = usable != nullptr;
    gst_plugin_feature_list_free(usable);
    gst_plugin_feature_list_free(decoders);
    return found;
}

}  // namespace

class LiveVideo {
public:
    LiveVideo(LivePost post, std::function<void(QImage)> picture, std::function<void(QString)> ended)
        : post_(std::move(post)), picture_(std::move(picture)), ended_(std::move(ended)),
          mailbox_(std::make_shared<Mailbox>()) {}

    ~LiveVideo() {
        if (pipeline_) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);  // waits for the pipeline's threads
            gst_object_unref(pipeline_);
        }
        if (src_) gst_object_unref(src_);
        if (bus_) gst_object_unref(bus_);
    }

    ptz_live *handle = nullptr;

    // The stream's thread: one frame from the core.
    void decode(const ptz_frame &frame) {
        if (failed_ || !frame.data || !frame.size) return;
        if (!pipeline_ && !build(frame.codec)) return;
        GstBuffer *buffer = gst_buffer_new_memdup(frame.data, frame.size);
        GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(frame.time * GST_SECOND);
        if (!frame.keyframe) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
        gst_app_src_push_buffer(GST_APP_SRC(src_), buffer);
        if (GstMessage *message = gst_bus_pop_filtered(bus_, GST_MESSAGE_ERROR)) {
            GError *e = nullptr;
            gst_message_parse_error(message, &e, nullptr);
            QString detail = e ? QString::fromUtf8(e->message) : QString("unknown error");
            if (e) g_error_free(e);
            gst_message_unref(message);
            fail("The camera's " + codec_name_ + " video can't be decoded (" + detail + ")");
        }
    }

    // The stream's thread: the core's stream ended by itself.
    void ended(const QString &reason) {
        auto ended = ended_;
        post_([ended, reason] { ended(reason); });
    }

private:
    bool build(ptz_codec codec) {
        bool h265 = codec == PTZ_CODEC_H265;
        codec_name_ = h265 ? "H.265" : "H.264";
        QString error;
        if (!gstreamer_ready(&error)) {
            fail(error);
            return false;
        }
        GstCaps *caps = gst_caps_from_string(h265 ? "video/x-h265,stream-format=byte-stream,alignment=au"
                                                  : "video/x-h264,stream-format=byte-stream,alignment=au");
        if (!has_decoder(caps)) {
            gst_caps_unref(caps);
            fail("This system has no " + codec_name_ + " decoder for the live view");
            return false;
        }
        // libav's software decoder when there is one (the codecs extension brings it), else whatever decodebin finds.
        const char *preferred = h265 ? "avdec_h265" : "avdec_h264";
        GstElementFactory *factory = gst_element_factory_find(preferred);
        QString decoder = factory ? preferred : "decodebin";
        if (factory) gst_object_unref(factory);
        QString description = QString("appsrc name=src ! %1 ! %2 ! videoconvert ! video/x-raw,format=BGRx ! "
                                      "appsink name=sink max-buffers=1 drop=true sync=false")
                                  .arg(h265 ? "h265parse" : "h264parse", decoder);
        GError *e = nullptr;
        GstElement *pipeline = gst_parse_launch(description.toUtf8().constData(), &e);
        if (!pipeline || e) {
            QString detail = e ? QString::fromUtf8(e->message) : QString("unknown error");
            if (e) g_error_free(e);
            if (pipeline) gst_object_unref(pipeline);
            gst_caps_unref(caps);
            fail("The live view's " + codec_name_ + " decoder can't be set up (" + detail + ")");
            return false;
        }
        pipeline_ = pipeline;
        src_ = gst_bin_get_by_name(GST_BIN(pipeline_), "src");
        g_object_set(src_, "caps", caps, "is-live", TRUE, "format", GST_FORMAT_TIME, "max-bytes",
                     static_cast<guint64>(8 << 20), "block", FALSE, nullptr);
        gst_caps_unref(caps);
        GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
        GstAppSinkCallbacks callbacks{};
        callbacks.new_sample = [](GstAppSink *sink, gpointer self) -> GstFlowReturn {
            static_cast<LiveVideo *>(self)->pull(sink);
            return GST_FLOW_OK;
        };
        gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, this, nullptr);
        gst_object_unref(sink);
        bus_ = gst_element_get_bus(pipeline_);
        if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            fail("The live view's " + codec_name_ + " decoder can't start");
            return false;
        }
        return true;
    }

    // The pipeline's thread: a decoded picture.
    void pull(GstAppSink *sink) {
        GstSample *sample = gst_app_sink_pull_sample(sink);
        if (!sample) return;
        GstVideoInfo info;
        GstVideoFrame frame;
        if (gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) &&
            gst_video_frame_map(&frame, &info, gst_sample_get_buffer(sample), GST_MAP_READ)) {
            int width = GST_VIDEO_FRAME_WIDTH(&frame), height = GST_VIDEO_FRAME_HEIGHT(&frame);
            int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
            const auto *pixels = static_cast<const uchar *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
            // BGRx bytes are QImage's RGB32 on little-endian machines.
            QImage image = QImage(pixels, width, height, stride, QImage::Format_RGB32).copy();
            gst_video_frame_unmap(&frame);
            deliver(std::move(image));
        }
        gst_sample_unref(sample);
    }

    void deliver(QImage image) {
        bool schedule;
        {
            std::lock_guard<std::mutex> lock(mailbox_->mutex);
            mailbox_->newest = std::move(image);
            schedule = !mailbox_->delivering;
            mailbox_->delivering = true;
        }
        if (!schedule) return;
        auto mailbox = mailbox_;
        auto picture = picture_;
        post_([mailbox, picture] {
            QImage image;
            {
                std::lock_guard<std::mutex> lock(mailbox->mutex);
                image = std::move(mailbox->newest);
                mailbox->newest = QImage();
                mailbox->delivering = false;
            }
            if (!image.isNull()) picture(image);
        });
    }

    void fail(const QString &reason) {
        failed_ = true;
        ended(reason);
    }

    LivePost post_;
    std::function<void(QImage)> picture_;
    std::function<void(QString)> ended_;
    std::shared_ptr<Mailbox> mailbox_;
    GstElement *pipeline_ = nullptr, *src_ = nullptr;
    GstBus *bus_ = nullptr;
    QString codec_name_ = "H.264";
    bool failed_ = false;
};

namespace {

void on_frame(void *context, const ptz_frame *frame) {
    if (context && frame) static_cast<LiveVideo *>(context)->decode(*frame);
}

void on_ended(void *context, const char *reason) {
    if (context) static_cast<LiveVideo *>(context)->ended(reason ? QString::fromUtf8(reason) : QString("The live view ended"));
}

}  // namespace

LiveVideo *live_start(ptz_session *session, LivePost post, std::function<void(QImage)> picture,
                      std::function<void(QString)> ended, QString *description, QString *error) {
    auto live = std::make_unique<LiveVideo>(std::move(post), std::move(picture), std::move(ended));
    ptz_live_callbacks callbacks{live.get(), on_frame, on_ended};
    char text[128] = {}, failure[1024] = {};
    live->handle = ptz_live_start(session, &callbacks, text, sizeof text, failure, sizeof failure);
    if (!live->handle) {
        *error = QString::fromUtf8(failure);
        return nullptr;
    }
    *description = QString::fromUtf8(text);
    return live.release();
}

void live_stop(LiveVideo *live) {
    if (!live) return;
    ptz_live_stop(live->handle);  // no callbacks after this
    delete live;
}
