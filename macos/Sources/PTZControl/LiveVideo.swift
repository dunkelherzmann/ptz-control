import AppKit
import CoreMedia
import PTZCore
import VideoToolbox

/// Decodes the camera's live video with VideoToolbox. Frames come from the core on the stream's own thread in
/// Annex B form; the newest decoded picture is handed to the main thread, skipping any the screen can't keep up with.
final class LiveDecoder: @unchecked Sendable {
    private let onPicture: @MainActor (CGImage, String) -> Void
    private let onFailure: @MainActor (String) -> Void

    private var codec: ptz_codec = PTZ_CODEC_H264
    private var parameterSets: [Data] = []
    private var format: CMVideoFormatDescription?
    private var session: VTDecompressionSession?
    private var failed = false
    private var failures = 0

    private let lock = NSLock()
    private var newest: CGImage?
    private var delivering = false

    init(onPicture: @escaping @MainActor (CGImage, String) -> Void, onFailure: @escaping @MainActor (String) -> Void) {
        self.onPicture = onPicture
        self.onFailure = onFailure
    }

    deinit {
        if let session { VTDecompressionSessionInvalidate(session) }
    }

    /// One frame from the core: NAL units after 00 00 00 01 start codes.
    func decode(_ frame: ptz_frame) {
        guard !failed, let data = frame.data else { return }
        codec = frame.codec
        let units = Self.nalUnits(UnsafeBufferPointer(start: data, count: frame.size))
        var sets: [Data] = [], pictures: [Data] = []
        for unit in units where !unit.isEmpty {
            isParameterSet(unit.first!) ? sets.append(unit) : pictures.append(unit)
        }
        if !sets.isEmpty, sets != parameterSets {
            parameterSets = sets
            guard makeSession() else { return }
        }
        guard let session, let format, !pictures.isEmpty else { return }

        // VideoToolbox takes each NAL unit with a 4-byte big-endian length in front.
        var sample = Data()
        for unit in pictures {
            var length = UInt32(unit.count).bigEndian
            sample.append(Data(bytes: &length, count: 4))
            sample.append(unit)
        }
        var block: CMBlockBuffer?
        let size = sample.count
        guard CMBlockBufferCreateWithMemoryBlock(allocator: kCFAllocatorDefault, memoryBlock: nil, blockLength: size,
                                                 blockAllocator: kCFAllocatorDefault, customBlockSource: nil,
                                                 offsetToData: 0, dataLength: size, flags: 0,
                                                 blockBufferOut: &block) == noErr, let block else { return }
        sample.withUnsafeBytes { bytes in
            _ = CMBlockBufferReplaceDataBytes(with: bytes.baseAddress!, blockBuffer: block, offsetIntoDestination: 0,
                                              dataLength: size)
        }
        var buffer: CMSampleBuffer?
        var sampleSize = size
        guard CMSampleBufferCreateReady(allocator: kCFAllocatorDefault, dataBuffer: block, formatDescription: format,
                                        sampleCount: 1, sampleTimingEntryCount: 0, sampleTimingArray: nil,
                                        sampleSizeEntryCount: 1, sampleSizeArray: &sampleSize,
                                        sampleBufferOut: &buffer) == noErr, let buffer else { return }
        let status = VTDecompressionSessionDecodeFrame(session, sampleBuffer: buffer, flags: [], infoFlagsOut: nil) {
            [weak self] status, _, imageBuffer, _, _ in
            guard let self, status == noErr, let imageBuffer else { return }
            var image: CGImage?
            VTCreateCGImageFromCVPixelBuffer(imageBuffer, options: nil, imageOut: &image)
            if let image { self.deliver(image) }
        }
        if status == kVTInvalidSessionErr {
            _ = makeSession()  // after sleep or a GPU change; the next key frame decodes again
        } else if status != noErr {
            failures += 1
            if failures >= 30 { fail("The Mac can't decode the camera's video (VideoToolbox error \(status))") }
        } else {
            failures = 0
        }
    }

    private func isParameterSet(_ header: UInt8) -> Bool {
        codec == PTZ_CODEC_H265 ? (32...34).contains((header >> 1) & 0x3f) : [7, 8].contains(header & 0x1f)
    }

    private func makeSession() -> Bool {
        if let session { VTDecompressionSessionInvalidate(session) }
        session = nil
        format = nil
        var description: CMVideoFormatDescription?
        let status: OSStatus = withPointers(parameterSets) { pointers, sizes in
            if codec == PTZ_CODEC_H265 {
                return CMVideoFormatDescriptionCreateFromHEVCParameterSets(
                    allocator: kCFAllocatorDefault, parameterSetCount: pointers.count, parameterSetPointers: pointers,
                    parameterSetSizes: sizes, nalUnitHeaderLength: 4, extensions: nil, formatDescriptionOut: &description)
            }
            return CMVideoFormatDescriptionCreateFromH264ParameterSets(
                allocator: kCFAllocatorDefault, parameterSetCount: pointers.count, parameterSetPointers: pointers,
                parameterSetSizes: sizes, nalUnitHeaderLength: 4, formatDescriptionOut: &description)
        }
        guard status == noErr, let description else {
            fail("The camera's video settings can't be read (error \(status))")
            return false
        }
        let attributes = [kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA] as CFDictionary
        var created: VTDecompressionSession?
        let made = VTDecompressionSessionCreate(allocator: kCFAllocatorDefault, formatDescription: description,
                                                decoderSpecification: nil, imageBufferAttributes: attributes,
                                                outputCallback: nil, decompressionSessionOut: &created)
        guard made == noErr, let created else {
            fail("The Mac can't decode the camera's " + (codec == PTZ_CODEC_H265 ? "H.265" : "H.264")
                 + " video (VideoToolbox error \(made))")
            return false
        }
        format = description
        session = created
        return true
    }

    private func withPointers<R>(_ sets: [Data], _ body: ([UnsafePointer<UInt8>], [Int]) -> R) -> R {
        let copies = sets.map { data -> UnsafeMutablePointer<UInt8> in
            let pointer = UnsafeMutablePointer<UInt8>.allocate(capacity: data.count)
            data.copyBytes(to: pointer, count: data.count)
            return pointer
        }
        defer { copies.forEach { $0.deallocate() } }
        return body(copies.map { UnsafePointer($0) }, sets.map(\.count))
    }

    private func fail(_ reason: String) {
        failed = true
        onMainRunLoop { MainActor.assumeIsolated { self.onFailure(reason) } }
    }

    private func deliver(_ image: CGImage) {
        lock.lock()
        newest = image
        let schedule = !delivering
        delivering = true
        lock.unlock()
        guard schedule else { return }
        onMainRunLoop {
            self.lock.lock()
            let image = self.newest
            self.newest = nil
            self.delivering = false
            self.lock.unlock()
            let codec = self.codec == PTZ_CODEC_H265 ? "H.265" : "H.264"
            if let image { MainActor.assumeIsolated { self.onPicture(image, codec) } }
        }
    }

    /// Splits Annex B data at its start codes (00 00 01, with or without a leading zero).
    static func nalUnits(_ bytes: UnsafeBufferPointer<UInt8>) -> [Data] {
        var units: [Data] = []
        var start: Int?
        var i = 0
        let n = bytes.count
        while i + 2 < n {
            if bytes[i] == 0, bytes[i + 1] == 0, bytes[i + 2] == 1 {
                if let s = start {
                    var end = i
                    if end > s, bytes[end - 1] == 0 { end -= 1 }
                    units.append(Data(bytes[s..<end]))
                }
                i += 3
                start = i
            } else {
                i += 1
            }
        }
        if let s = start, s < n { units.append(Data(bytes[s..<n])) }
        return units
    }
}

/// The camera's live video for one connection: the core's stream plus its decoder.
final class LiveVideo: @unchecked Sendable {
    let decoder: LiveDecoder
    var handle: OpaquePointer?  // the link's queue only
    let onEnded: @MainActor (String) -> Void

    init(decoder: LiveDecoder, onEnded: @escaping @MainActor (String) -> Void) {
        self.decoder = decoder
        self.onEnded = onEnded
    }
}

let liveFrame: @convention(c) (UnsafeMutableRawPointer?, UnsafePointer<ptz_frame>?) -> Void = { context, frame in
    guard let context, let frame else { return }
    Unmanaged<LiveVideo>.fromOpaque(context).takeUnretainedValue().decoder.decode(frame.pointee)
}

let liveEnded: @convention(c) (UnsafeMutableRawPointer?, UnsafePointer<CChar>?) -> Void = { context, reason in
    guard let context else { return }
    let live = Unmanaged<LiveVideo>.fromOpaque(context).takeUnretainedValue()
    let text = reason.map { String(cString: $0) } ?? "The live view ended"
    onMainRunLoop { MainActor.assumeIsolated { live.onEnded(text) } }
}

extension CameraLink {
    /// Starts the camera's live video. Pictures arrive on the main thread; `ended` is called when it stops by
    /// itself or can't be decoded. The completion gets a description such as "H.264 over RTSP".
    func startLive(picture: @escaping @MainActor (CGImage, String) -> Void, ended: @escaping @MainActor (String) -> Void,
                   completion: @escaping @MainActor (Result<String, LinkError>) -> Void) {
        let decoder = LiveDecoder(onPicture: picture, onFailure: ended)
        let live = LiveVideo(decoder: decoder, onEnded: ended)
        runOnSession({ session in
            var description = [CChar](repeating: 0, count: 128)
            var error = [CChar](repeating: 0, count: 1024)
            var callbacks = ptz_live_callbacks(context: Unmanaged.passUnretained(live).toOpaque(),
                                               frame: liveFrame, ended: liveEnded)
            guard let handle = ptz_live_start(session, &callbacks, &description, description.count,
                                              &error, error.count) else {
                return .failure(LinkError(message: String(cString: error), linkLost: ptz_link_alive(session) == 0))
            }
            live.handle = handle
            self.live = live
            return .success(String(cString: description))
        }, completion: completion)
    }

    /// Stops the live video, if any, and waits for its thread.
    func stopLive(completion: @escaping @MainActor () -> Void = {}) {
        runOnSession({ _ in
            self.stopLiveNow()
            return .success(())
        }, completion: { _ in completion() })
    }
}
