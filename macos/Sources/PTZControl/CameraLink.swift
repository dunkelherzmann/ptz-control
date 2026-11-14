import AppKit
import PTZCore

struct AxisRange: Equatable {
    let min: Double
    let max: Double

    func fraction(_ value: Double) -> Double? {
        guard max > min, value >= min, value <= max else { return nil }
        return (value - min) / (max - min)
    }

    func value(at fraction: Double) -> Double { min + (max - min) * Swift.min(1, Swift.max(0, fraction)) }
}

struct CameraInfo {
    let name: String
    let units: String
    let pan: AxisRange?
    let tilt: AxisRange?
    let zoom: AxisRange?
    let zoomInteger: Bool
    let viewMoves: Bool  // a point of the picture can be brought to its centre
}

/// A measured value; `nil` readings mean unavailable, never zero.
struct Reading: Equatable {
    let value: Double
    let text: String
}

struct PositionSnapshot: Equatable {
    var pan: Reading?
    var tilt: Reading?
    var zoom: Reading?
    var moveStatus: String?

    var isEmpty: Bool { pan == nil && tilt == nil && zoom == nil }
}

struct Target {
    var pan: Double?
    var tilt: Double?
    var zoom: Double?

    var isEmpty: Bool { pan == nil && tilt == nil && zoom == nil }
}

enum Route: Int, Codable { case local = 0, ssh = 1 }
enum Vendor: Int, Codable { case onvif = 0, axis = 1 }

/// Everything about a connection except secrets, which stay in memory only.
struct ConnectionSettings: Codable, Equatable {
    var route: Route = .local
    var vendor: Vendor = .onvif
    var cameraHost = ""
    var cameraPort = 80
    var cameraUser = ProcessInfo.processInfo.environment["PTZ_USER"] ?? "admin"
    var httpsPort = 443
    var profile = 0
    var timeout = 15.0
    var sshHost = ""
    var sshUser = ""
    var sshPort = 22
    var sshIdentity = ""

    static let defaultsKey = "ConnectionSettings"

    static func load() -> ConnectionSettings {
        guard let data = UserDefaults.standard.data(forKey: defaultsKey),
              let settings = try? JSONDecoder().decode(ConnectionSettings.self, from: data) else { return .init() }
        return settings
    }

    func save() {
        if let data = try? JSONEncoder().encode(self) { UserDefaults.standard.set(data, forKey: Self.defaultsKey) }
    }
}

struct LinkError: LocalizedError {
    let message: String
    var linkLost = false
    var errorDescription: String? { message }
}

/// Operator interaction the core may request while connecting.
@MainActor protocol CameraLinkDelegate: AnyObject {
    func cameraLink(_ link: CameraLink, log line: String)
    func cameraLink(_ link: CameraLink, prompt message: String, secret: Bool) -> String?
    func cameraLink(_ link: CameraLink, confirm title: String, message: String) -> Bool
}

/// Owns one core session. All core calls run on one serial queue, so camera
/// requests never overlap; completions are delivered on the main thread.
final class CameraLink: @unchecked Sendable {
    weak var delegate: CameraLinkDelegate?

    private let queue = DispatchQueue(label: "ptz.camera", qos: .userInitiated)
    private var session: OpaquePointer?
    private let cancelFlag = UnsafeMutablePointer<Int32>.allocate(capacity: 1)
    private(set) var pending = 0  // main thread only
    var live: LiveVideo?  // the link's queue only

    init() { cancelFlag.initialize(to: 0) }

    deinit {
        if let handle = live?.handle { ptz_live_stop(handle) }
        if let session { ptz_close(session) }
        cancelFlag.deallocate()
    }

    var isBusy: Bool { pending > 0 }

    private func run<T>(_ work: @escaping () -> Result<T, LinkError>,
                        completion: @escaping @MainActor (Result<T, LinkError>) -> Void) {
        pending += 1
        queue.async {
            let result = work()
            onMainRunLoop {
                self.pending -= 1
                MainActor.assumeIsolated { completion(result) }
            }
        }
    }

    /// Runs `work` with the session on the link's queue.
    func runOnSession<T>(_ work: @escaping (OpaquePointer) -> Result<T, LinkError>,
                         completion: @escaping @MainActor (Result<T, LinkError>) -> Void) {
        run({
            switch self.requireSession() {
            case .failure(let e): return .failure(e)
            case .success(let session): return work(session)
            }
        }, completion: completion)
    }

    /// The link's queue only.
    func stopLiveNow() {
        if let handle = live?.handle { ptz_live_stop(handle) }
        live = nil
    }

    private func requireSession() -> Result<OpaquePointer, LinkError> {
        guard let session else { return .failure(LinkError(message: "Not connected")) }
        return .success(session)
    }

    private func failure<T>(_ buffer: [CChar], session: OpaquePointer) -> Result<T, LinkError> {
        .failure(LinkError(message: String(cString: buffer), linkLost: ptz_link_alive(session) == 0))
    }

    func connect(_ settings: ConnectionSettings, cameraPassword: String, sshPassword: String,
                 completion: @escaping @MainActor (Result<CameraInfo, LinkError>) -> Void) {
        cancelFlag.pointee = 0
        let askpass = Bundle.main.executablePath ?? CommandLine.arguments[0]
        let context = Unmanaged.passUnretained(self).toOpaque()
        run({ [cancelFlag] in
            let strings = CStrings()
            var params = ptz_connect_params()
            params.route = settings.route == .ssh ? PTZ_ROUTE_SSH : PTZ_ROUTE_LOCAL
            params.vendor = settings.vendor == .axis ? PTZ_VENDOR_AXIS : PTZ_VENDOR_ONVIF
            params.camera_host = strings.add(settings.cameraHost)
            params.camera_port = Int32(clamping: settings.cameraPort)
            params.camera_user = strings.add(settings.cameraUser)
            params.camera_password = strings.add(cameraPassword, secret: true)
            params.https_port = Int32(clamping: settings.httpsPort)
            params.profile = Int32(clamping: settings.profile)
            params.timeout = settings.timeout
            params.ssh_host = strings.add(settings.sshHost)
            params.ssh_user = strings.add(settings.sshUser)
            params.ssh_password = strings.add(sshPassword, secret: true)
            params.ssh_port = Int32(clamping: settings.sshPort)
            params.ssh_identity = strings.add((settings.sshIdentity as NSString).expandingTildeInPath)
            params.askpass_path = strings.add(askpass)
            var callbacks = ptz_callbacks(context: context, log: linkLog, prompt: linkPrompt, confirm: linkConfirm)
            var error = [CChar](repeating: 0, count: 1024)
            guard let session = ptz_connect(&params, &callbacks, cancelFlag, &error, error.count) else {
                return .failure(LinkError(message: String(cString: error)))
            }
            self.session = session
            var info = ptz_info()
            ptz_info_get(session, &info)
            func range(_ r: ptz_range) -> AxisRange? { r.available != 0 ? AxisRange(min: r.min, max: r.max) : nil }
            return .success(CameraInfo(
                name: withUnsafeBytes(of: info.name) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) },
                units: withUnsafeBytes(of: info.units) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) },
                pan: range(info.pan), tilt: range(info.tilt), zoom: range(info.zoom),
                zoomInteger: info.zoom_integer != 0, viewMoves: info.view_moves != 0))
        }, completion: completion)
    }

    func cancelConnect() { cancelFlag.pointee = 1 }

    func position(completion: @escaping @MainActor (Result<PositionSnapshot, LinkError>) -> Void) {
        run({
            switch self.requireSession() {
            case .failure(let e): return .failure(e)
            case .success(let session):
                var position = ptz_position()
                var error = [CChar](repeating: 0, count: 1024)
                guard ptz_get_position(session, &position, &error, error.count) != 0 else {
                    return self.failure(error, session: session)
                }
                func reading(_ r: ptz_reading) -> Reading? {
                    guard r.available != 0 else { return nil }
                    let text = withUnsafeBytes(of: r.text) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
                    return Reading(value: r.value, text: text)
                }
                let status = withUnsafeBytes(of: position.move_status) {
                    String(cString: $0.bindMemory(to: CChar.self).baseAddress!)
                }
                return .success(PositionSnapshot(pan: reading(position.pan), tilt: reading(position.tilt),
                                                 zoom: reading(position.zoom),
                                                 moveStatus: status.isEmpty ? nil : status))
            }
        }, completion: completion)
    }

    func move(_ target: Target, completion: @escaping @MainActor (Result<Void, LinkError>) -> Void) {
        run({
            switch self.requireSession() {
            case .failure(let e): return .failure(e)
            case .success(let session):
                var t = ptz_target()
                if let pan = target.pan { t.has_pan = 1; t.pan = pan }
                if let tilt = target.tilt { t.has_tilt = 1; t.tilt = tilt }
                if let zoom = target.zoom { t.has_zoom = 1; t.zoom = zoom }
                var error = [CChar](repeating: 0, count: 1024)
                return ptz_move(session, &t, &error, error.count) != 0 ? .success(()) : self.failure(error, session: session)
            }
        }, completion: completion)
    }

    /// A still picture of the camera's current view.
    func picture(completion: @escaping @MainActor (Result<NSImage, LinkError>) -> Void) {
        run({
            switch self.requireSession() {
            case .failure(let e): return .failure(e)
            case .success(let session):
                var data: UnsafeMutablePointer<UInt8>?
                var size = 0
                var width: Int32 = 0, height: Int32 = 0
                var error = [CChar](repeating: 0, count: 1024)
                guard ptz_picture(session, &data, &size, &width, &height, &error, error.count) != 0, let data else {
                    return self.failure(error, session: session)
                }
                let bytes = Data(bytes: data, count: size)
                ptz_free(data)
                return .success((bytes, NSSize(width: Int(width), height: Int(height))))
            }
        }, completion: { (result: Result<(Data, NSSize), LinkError>) in
            completion(result.flatMap { bytes, size in
                guard let image = NSImage(data: bytes) else {
                    return .failure(LinkError(message: "The camera's picture could not be read"))
                }
                image.size = size  // pixels, whatever the file's DPI
                return .success(image)
            })
        })
    }

    /// Brings the point (x, y) of the last picture to its centre; x and y run from −1 to 1, up positive.
    func moveInView(x: Double, y: Double, completion: @escaping @MainActor (Result<Void, LinkError>) -> Void) {
        run({
            switch self.requireSession() {
            case .failure(let e): return .failure(e)
            case .success(let session):
                var error = [CChar](repeating: 0, count: 1024)
                return ptz_move_in_view(session, x, y, &error, error.count) != 0
                    ? .success(()) : self.failure(error, session: session)
            }
        }, completion: completion)
    }

    func stop(completion: @escaping @MainActor (Result<Void, LinkError>) -> Void) {
        run({
            switch self.requireSession() {
            case .failure(let e): return .failure(e)
            case .success(let session):
                var error = [CChar](repeating: 0, count: 1024)
                return ptz_stop(session, &error, error.count) != 0 ? .success(()) : self.failure(error, session: session)
            }
        }, completion: completion)
    }

    func webURL(completion: @escaping @MainActor (Result<(URL, String), LinkError>) -> Void) {
        run({
            switch self.requireSession() {
            case .failure(let e): return .failure(e)
            case .success(let session):
                var url = [CChar](repeating: 0, count: 2048)
                var notice = [CChar](repeating: 0, count: 512)
                var error = [CChar](repeating: 0, count: 1024)
                guard ptz_web_url(session, &url, url.count, &notice, notice.count, &error, error.count) != 0 else {
                    return self.failure(error, session: session)
                }
                guard let parsed = URL(string: String(cString: url)), ["http", "https"].contains(parsed.scheme) else {
                    return .failure(LinkError(message: "Camera web interface address is invalid"))
                }
                return .success((parsed, String(cString: notice)))
            }
        }, completion: completion)
    }

    /// Waits for any in-flight request; no new moves are queued after this.
    func disconnect(completion: @escaping @MainActor () -> Void) {
        run({ () -> Result<Void, LinkError> in
            self.stopLiveNow()
            if let session = self.session {
                ptz_close(session)
                self.session = nil
            }
            return .success(())
        }, completion: { _ in completion() })
    }

    // Core callbacks arrive on core threads; the delegate is asked on main.
    fileprivate func log(_ line: String) {
        onMainRunLoop { MainActor.assumeIsolated { self.delegate?.cameraLink(self, log: line) } }
    }

    fileprivate func prompt(_ message: String, secret: Bool) -> String? {
        onMainRunLoopAndWait { self.delegate?.cameraLink(self, prompt: message, secret: secret) }
    }

    fileprivate func confirm(_ title: String, _ message: String) -> Bool {
        onMainRunLoopAndWait { self.delegate?.cameraLink(self, confirm: title, message: message) ?? false }
    }
}

/// Runs on the main thread from the run loop itself, so completions also
/// arrive while a nested loop is active (quit-reply waits, modal alerts),
/// even when that loop was entered from a main-queue block.
func onMainRunLoop(_ block: @escaping () -> Void) {
    let modes = [CFRunLoopMode.commonModes.rawValue, RunLoop.Mode.modalPanel.rawValue as CFString] as CFArray
    CFRunLoopPerformBlock(CFRunLoopGetMain(), modes, block)
    CFRunLoopWakeUp(CFRunLoopGetMain())
}

/// Blocks a core thread while the operator answers on the main thread.
func onMainRunLoopAndWait<T>(_ body: @escaping @MainActor () -> T) -> T {
    let done = DispatchSemaphore(value: 0)
    var result: T?
    onMainRunLoop {
        result = MainActor.assumeIsolated { body() }
        done.signal()
    }
    done.wait()
    return result!
}

private func link(_ context: UnsafeMutableRawPointer?) -> CameraLink {
    Unmanaged<CameraLink>.fromOpaque(context!).takeUnretainedValue()
}

private let linkLog: @convention(c) (UnsafeMutableRawPointer?, UnsafePointer<CChar>?) -> Void = { context, line in
    link(context).log(String(cString: line!))
}

private let linkPrompt: @convention(c) (UnsafeMutableRawPointer?, UnsafePointer<CChar>?, Int32,
                                        UnsafeMutablePointer<CChar>?, Int) -> Int32 = { context, message, secret, answer, size in
    guard let reply = link(context).prompt(String(cString: message!), secret: secret != 0), let answer else { return 0 }
    let bytes = Array(reply.utf8.prefix(size - 1))
    for (index, byte) in bytes.enumerated() { answer[index] = CChar(bitPattern: byte) }
    answer[bytes.count] = 0
    return 1
}

private let linkConfirm: @convention(c) (UnsafeMutableRawPointer?, UnsafePointer<CChar>?,
                                         UnsafePointer<CChar>?) -> Int32 = { context, title, message in
    link(context).confirm(String(cString: title!), String(cString: message!)) ? 1 : 0
}

/// C strings for one core call; secrets are wiped before release.
private final class CStrings {
    private var storage: [(UnsafeMutablePointer<CChar>, Bool)] = []

    func add(_ text: String, secret: Bool = false) -> UnsafePointer<CChar> {
        let pointer = strdup(text)!
        storage.append((pointer, secret))
        return UnsafePointer(pointer)
    }

    deinit {
        for (pointer, secret) in storage {
            if secret { memset(pointer, 0, strlen(pointer)) }
            free(pointer)
        }
    }
}
