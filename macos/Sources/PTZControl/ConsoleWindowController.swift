import AppKit
import PTZCore

/// The single console window: link configuration on the left, live
/// instruments, target entry and the event buffer on the right.
@MainActor
final class ConsoleWindowController: NSWindowController, NSWindowDelegate, CameraLinkDelegate {
    enum State { case idle, connecting, connected, disconnecting }

    private let link = CameraLink()
    private(set) var state = State.idle
    private var settings = ConnectionSettings.load()
    private var info: CameraInfo?
    private var starting: PositionSnapshot?
    private var current: PositionSnapshot?
    private var failures = 0
    private(set) var paused = false
    private var refreshTimer: Timer?
    private var nextRefresh = Date.distantPast
    private var pendingWebOpen = false
    var theme: PhosphorTheme = PhosphorTheme(rawValue: UserDefaults.standard.string(forKey: "Theme") ?? "") ?? .system {
        didSet {
            UserDefaults.standard.set(theme.rawValue, forKey: "Theme")
            applyTheme()
        }
    }

    // Views
    private let root = ThemedBackgroundView()
    private let connection = ConnectionPanel()
    private let plane = PositionPlaneView()
    private let panMeter = AxisMeterView()
    private let tiltMeter = AxisMeterView()
    private let zoomMeter = AxisMeterView()
    private let panReadout = ReadoutView(title: "PAN")
    private let tiltReadout = ReadoutView(title: "TILT")
    private let zoomReadout = ReadoutView(title: "ZOOM")
    private let statusReadout = ReadoutView(title: "MOTION")
    private let startReadout = NSTextField(wrappingLabelWithString: "Starting: —")
    private let headerTitle = NSTextField(labelWithString: "No camera link")
    private let headerUnits = NSTextField(labelWithString: "Choose a route and vendor, enter the camera details, then Connect.")
    private let statusChip = StatusChip()
    private let targetPan = NSTextField()
    private let targetTilt = NSTextField()
    private let targetZoom = NSTextField()
    private let moveButton = NSButton(title: "Move", target: nil, action: nil)
    private let startButton = NSButton(title: "Return to Start", target: nil, action: nil)
    private let stopButton = NSButton(title: "Stop", target: nil, action: nil)
    private let webButton = NSButton(title: "Web Interface", target: nil, action: nil)
    private let eventLog = EventLogView()
    private let footer = NSTextField(labelWithString: "")
    private var planePanel: PanelView!
    private var themedLabels: [(NSTextField, (PhosphorTheme) -> NSColor)] = []

    init() {
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1180, height: 780),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable],
                              backing: .buffered, defer: false)
        window.title = "PTZ Control"
        window.subtitle = "PTZ Coordinates Controller by Herbert Cisneros"
        window.minSize = NSSize(width: 980, height: 680)
        window.setFrameAutosaveName("ConsoleWindow")
        window.tabbingMode = .disallowed
        super.init(window: window)
        window.delegate = self
        link.delegate = self
        build()
        connection.load(settings)
        updateControls()
        applyTheme()
        if window.frame.origin == .zero { window.center() }
        eventLog.append("PTZ Control ready · core \(String(cString: ptz_core_version()))")
        refreshTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.tick() }
        }
    }

    required init?(coder: NSCoder) { fatalError() }

    // MARK: Layout

    private func build() {
        guard let window else { return }
        window.contentView = root

        let sidebar = PanelView(caption: "Secure link configuration", content: connection)
        connection.onConnect = { [weak self] in self?.toggleConnection() }

        headerTitle.font = .systemFont(ofSize: 17, weight: .semibold)
        headerUnits.font = .systemFont(ofSize: 12)
        headerUnits.lineBreakMode = .byTruncatingTail
        themedLabels += [(headerTitle, { $0.text }), (headerUnits, { $0.secondary }), (footer, { $0.secondary }),
                         (startReadout, { $0.start })]
        let titles = NSStackView(views: [headerTitle, headerUnits])
        titles.orientation = .vertical
        titles.alignment = .leading
        titles.spacing = 2
        let header = NSStackView(views: [titles, NSView(), statusChip])
        header.orientation = .horizontal
        header.alignment = .centerY
        header.distribution = .fill
        titles.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)

        plane.onAim = { [weak self] pan, tilt in self?.aim(pan: pan, tilt: tilt) }
        plane.onAimAndMove = { [weak self] pan, tilt in
            self?.aim(pan: pan, tilt: tilt)
            self?.sendMove()
        }
        planePanel = PanelView(caption: "Position plane // XY — click to aim, double-click to move", content: plane)

        for (meter, field) in [(panMeter, targetPan), (tiltMeter, targetTilt), (zoomMeter, targetZoom)] {
            meter.onAim = { [weak self] value in
                field.stringValue = format(value)
                self?.updateTargetMarkers()
            }
            meter.onAimAndMove = { [weak self] value in
                field.stringValue = format(value)
                self?.sendMove()
            }
        }
        panMeter.title = "PAN"
        tiltMeter.title = "TILT"
        zoomMeter.title = "ZOOM"
        let meters = NSStackView(views: [panMeter, tiltMeter, zoomMeter])
        meters.orientation = .vertical
        meters.spacing = 8
        meters.distribution = .fillEqually
        for m in [panMeter, tiltMeter, zoomMeter] { m.widthAnchor.constraint(equalTo: meters.widthAnchor).isActive = true }
        let metersPanel = PanelView(caption: "Axis travel // % of advertised limits — drag to aim, double-click to move", content: meters)

        startReadout.font = Fonts.mono(11)
        let readoutFill = NSView()
        readoutFill.setContentHuggingPriority(.init(1), for: .vertical)
        let readouts = NSStackView(views: [panReadout, tiltReadout, zoomReadout, statusReadout, startReadout, readoutFill])
        readouts.orientation = .vertical
        readouts.alignment = .leading
        readouts.spacing = 12
        for view in [panReadout, tiltReadout, zoomReadout, statusReadout, startReadout] as [NSView] {
            view.setContentHuggingPriority(.required, for: .vertical)
            view.widthAnchor.constraint(equalTo: readouts.widthAnchor).isActive = true
        }
        let readoutPanel = PanelView(caption: "Current coordinates", content: readouts)

        for (field, name) in [(targetPan, "Pan"), (targetTilt, "Tilt"), (targetZoom, "Zoom")] {
            field.placeholderString = "\(name): unchanged"
            field.font = Fonts.mono(13)
            field.usesSingleLineMode = true
            field.target = self
            field.action = #selector(targetReturn(_:))
            field.delegate = self
            field.widthAnchor.constraint(equalToConstant: 130).isActive = true
        }
        moveButton.keyEquivalent = "\r"
        moveButton.keyEquivalentModifierMask = .command
        for (button, action) in [(moveButton, #selector(moveAction(_:))), (startButton, #selector(returnToStart(_:))),
                                 (stopButton, #selector(stopAction(_:))), (webButton, #selector(openWebInterface(_:)))] {
            button.target = self
            button.action = action
            button.bezelStyle = .push
        }
        stopButton.contentTintColor = .systemRed
        let targetFill = NSView()
        targetFill.setContentHuggingPriority(.init(1), for: .horizontal)
        let targetRow = NSStackView(views: [targetPan, targetTilt, targetZoom, moveButton, targetFill, startButton, stopButton, webButton])
        targetRow.orientation = .horizontal
        targetRow.spacing = 8
        let targetNote = NSTextField(labelWithString: "Absolute targets, not increments. Leave a field blank (or x) to keep that axis.")
        targetNote.font = .systemFont(ofSize: 11)
        themedLabels.append((targetNote, { $0.secondary }))
        let targetStack = NSStackView(views: [targetRow, targetNote])
        targetStack.orientation = .vertical
        targetStack.alignment = .leading
        targetStack.spacing = 6
        targetRow.widthAnchor.constraint(equalTo: targetStack.widthAnchor).isActive = true
        let targetPanel = PanelView(caption: "Target // absolute move", content: targetStack)

        let logPanel = PanelView(caption: "Event buffer // session", content: eventLog)

        footer.font = Fonts.mono(10)

        let views: [NSView] = [sidebar, header, planePanel, metersPanel, readoutPanel, targetPanel, logPanel, footer]
        for v in views {
            v.translatesAutoresizingMaskIntoConstraints = false
            root.addSubview(v)
        }
        let gap: CGFloat = 12
        NSLayoutConstraint.activate([
            sidebar.topAnchor.constraint(equalTo: root.topAnchor, constant: gap),
            sidebar.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: gap),
            sidebar.bottomAnchor.constraint(equalTo: footer.topAnchor, constant: -8),
            sidebar.widthAnchor.constraint(equalToConstant: 300),

            header.topAnchor.constraint(equalTo: root.topAnchor, constant: gap),
            header.leadingAnchor.constraint(equalTo: sidebar.trailingAnchor, constant: gap + 4),
            header.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -gap - 4),

            planePanel.topAnchor.constraint(equalTo: header.bottomAnchor, constant: gap),
            planePanel.leadingAnchor.constraint(equalTo: sidebar.trailingAnchor, constant: gap),
            planePanel.trailingAnchor.constraint(equalTo: readoutPanel.leadingAnchor, constant: -gap),
            planePanel.heightAnchor.constraint(greaterThanOrEqualToConstant: 240),

            readoutPanel.topAnchor.constraint(equalTo: planePanel.topAnchor),
            readoutPanel.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -gap),
            readoutPanel.widthAnchor.constraint(equalToConstant: 230),
            readoutPanel.bottomAnchor.constraint(equalTo: metersPanel.bottomAnchor),

            metersPanel.topAnchor.constraint(equalTo: planePanel.bottomAnchor, constant: gap),
            metersPanel.leadingAnchor.constraint(equalTo: planePanel.leadingAnchor),
            metersPanel.trailingAnchor.constraint(equalTo: planePanel.trailingAnchor),
            metersPanel.heightAnchor.constraint(equalToConstant: 148),

            targetPanel.topAnchor.constraint(equalTo: metersPanel.bottomAnchor, constant: gap),
            targetPanel.leadingAnchor.constraint(equalTo: planePanel.leadingAnchor),
            targetPanel.trailingAnchor.constraint(equalTo: readoutPanel.trailingAnchor),

            logPanel.topAnchor.constraint(equalTo: targetPanel.bottomAnchor, constant: gap),
            logPanel.leadingAnchor.constraint(equalTo: planePanel.leadingAnchor),
            logPanel.trailingAnchor.constraint(equalTo: readoutPanel.trailingAnchor),
            logPanel.bottomAnchor.constraint(equalTo: footer.topAnchor, constant: -8),
            logPanel.heightAnchor.constraint(equalToConstant: 150),

            footer.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: gap + 4),
            footer.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -gap),
            footer.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -8),
        ])
    }

    private func applyTheme() {
        guard let window else { return }
        window.appearance = theme.appearance
        root.color = theme.background
        root.applyThemeRecursively(theme)
        for (label, color) in themedLabels { label.textColor = color(theme) }
        statusChip.theme = theme
        eventLog.theme = theme
    }

    // MARK: Connection

    @objc func toggleConnection(_ sender: Any? = nil) {
        switch state {
        case .idle: connect()
        case .connecting:
            link.cancelConnect()
            eventLog.append("Cancelling connection…")
        case .connected: disconnect()
        case .disconnecting: break
        }
    }

    private func connect() {
        // Commit the field being edited (often the camera password) and capture
        // the secrets before the form is disabled: disabling a field that is
        // still being edited discards its uncommitted text.
        window?.makeFirstResponder(nil)
        let cameraPassword = connection.cameraPassword.stringValue
        let sshPassword = connection.sshPassword.stringValue
        let s: ConnectionSettings
        do { s = try connection.settings() } catch {
            connection.show(message: error.localizedDescription, error: true)
            return
        }
        if cameraPassword.isEmpty {
            connection.show(message: "Camera password is required", error: true)
            window?.makeFirstResponder(connection.cameraPassword)
            return
        }
        settings = s
        s.save()
        state = .connecting
        connection.show(message: s.route == .ssh ? "Opening SSH tunnel…" : "Connecting…", error: false)
        eventLog.append("Connecting: \(s.route == .ssh ? "SSH via \(s.sshUser)@\(s.sshHost)" : "local") → \(s.cameraHost):\(s.cameraPort) · \(s.vendor == .axis ? "Axis VAPIX" : "ONVIF")")
        updateControls()
        link.connect(s, cameraPassword: cameraPassword,
                     sshPassword: s.route == .ssh ? sshPassword : "") { [weak self] result in
            guard let self else { return }
            switch result {
            case .success(let info):
                self.info = info
                self.state = .connected
                self.starting = nil
                self.current = nil
                self.failures = 0
                self.paused = false
                self.plane.resetTrail()
                self.connection.show(message: "Connected. No movement has been sent.", error: false)
                self.eventLog.append("Connected: \(info.name)")
                self.eventLog.append(info.units)
                for (axis, range) in [("Pan", info.pan), ("Tilt", info.tilt), ("Zoom", info.zoom)] {
                    self.eventLog.append("  \(axis) range: " + (range.map { "\(format($0.min)) … \(format($0.max))" } ?? "not advertised"))
                }
                self.refresh()
            case .failure(let error):
                self.state = .idle
                self.connection.show(message: error.message, error: true)
                self.eventLog.append("Connection failed: \(error.message)", warning: true)
            }
            self.updateControls()
        }
    }

    func disconnect(then: (() -> Void)? = nil) {
        guard state == .connected || state == .connecting else { then?(); return }
        if state == .connecting { link.cancelConnect() }
        state = .disconnecting
        updateControls()
        eventLog.append("Disconnecting; waiting for any in-flight request (no PTZ stop is sent)")
        link.disconnect { [weak self] in
            guard let self else { return }
            self.state = .idle
            self.info = nil
            self.current = nil
            self.starting = nil
            self.connection.show(message: "Disconnected.", error: false)
            self.eventLog.append("Disconnected")
            self.updateControls()
            then?()
        }
    }

    private func linkLost(_ message: String) {
        eventLog.append(message, warning: true)
        connection.show(message: message, error: true)
        disconnect()
    }

    // MARK: Position refresh

    private func tick() {
        guard state == .connected, !paused, !link.isBusy, Date() >= nextRefresh else { return }
        refresh()
    }

    @objc func refreshNow(_ sender: Any?) {
        guard state == .connected else { return }
        if paused || failures > 0 { eventLog.append("Auto-refresh resumed") }
        failures = 0
        paused = false
        nextRefresh = .distantPast
        updateControls()
        if !link.isBusy { refresh() }
    }

    @objc func togglePause(_ sender: Any?) {
        guard state == .connected else { return }
        paused.toggle()
        eventLog.append(paused ? "Auto-refresh paused" : "Auto-refresh resumed")
        updateControls()
    }

    private func refresh() {
        link.position { [weak self] result in
            guard let self, self.state == .connected else { return }
            self.nextRefresh = Date().addingTimeInterval(2)
            switch result {
            case .success(let snapshot):
                self.failures = 0
                if self.starting == nil {
                    self.starting = snapshot
                    self.eventLog.append("Starting coordinates: \(Self.describe(snapshot))")
                }
                if snapshot != self.current, self.current != nil { self.eventLog.append("Current coordinates: \(Self.describe(snapshot))") }
                self.current = snapshot
            case .failure(let error):
                if error.linkLost { self.linkLost(error.message); return }
                self.failures += 1
                self.eventLog.append("Read failed: \(error.message)", warning: true)
                if self.starting == nil { self.starting = PositionSnapshot() }
                if self.failures >= 3 {
                    self.paused = true
                    self.eventLog.append("Auto-refresh paused after 3 failures. Press ⌘R to retry.", warning: true)
                }
            }
            self.updateControls()
        }
    }

    static func describe(_ s: PositionSnapshot) -> String {
        "Pan:\(s.pan?.text ?? "unknown") Tilt:\(s.tilt?.text ?? "unknown") Zoom:\(s.zoom?.text ?? "unknown")"
            + (s.moveStatus.map { " (\($0))" } ?? "")
    }

    // MARK: Moves

    private func aim(pan: Double, tilt: Double) {
        targetPan.stringValue = format(pan)
        targetTilt.stringValue = format(tilt)
        updateTargetMarkers()
    }

    private func parseTarget() throws -> Target {
        func axis(_ field: NSTextField, _ name: String) throws -> Double? {
            let text = field.stringValue.trimmingCharacters(in: .whitespaces)
            if text.isEmpty || text.lowercased() == "x" { return nil }
            var value = 0.0
            guard ptz_parse(text, &value) else { throw LinkError(message: "\(name) must be a number (or blank to keep it)") }
            return value
        }
        return Target(pan: try axis(targetPan, "Pan"), tilt: try axis(targetTilt, "Tilt"), zoom: try axis(targetZoom, "Zoom"))
    }

    private func updateTargetMarkers() {
        let t = try? parseTarget()
        plane.target = (t?.pan, t?.tilt)
        panMeter.target = t?.pan
        tiltMeter.target = t?.tilt
        zoomMeter.target = t?.zoom
    }

    @objc private func targetReturn(_ sender: NSTextField) { sendMove() }
    @objc func moveAction(_ sender: Any?) { sendMove() }

    private func sendMove() {
        guard state == .connected else { return }
        do {
            let target = try parseTarget()
            if target.isEmpty { eventLog.append("All axes unchanged; no move sent."); return }
            send(target, describe: "Move")
        } catch {
            eventLog.append(error.localizedDescription, warning: true)
            NSSound.beep()
        }
    }

    @objc func returnToStart(_ sender: Any?) {
        guard state == .connected else { return }
        guard let s = starting, !s.isEmpty else {
            eventLog.append("Starting coordinates unavailable; no return move sent", warning: true)
            return
        }
        let missing = [("pan", s.pan), ("tilt", s.tilt), ("zoom", s.zoom)].filter { $0.1 == nil }.map(\.0)
        var note = "Returning to starting coordinates"
        if !missing.isEmpty { note += "; unavailable axes left unchanged: " + missing.joined(separator: ", ") }
        eventLog.append(note)
        // Show the start position as the target, so the plane and meter handles move there too.
        targetPan.stringValue = s.pan?.text ?? ""
        targetTilt.stringValue = s.tilt?.text ?? ""
        targetZoom.stringValue = s.zoom?.text ?? ""
        updateTargetMarkers()
        send(Target(pan: s.pan?.value, tilt: s.tilt?.value, zoom: s.zoom?.value), describe: "Return")
    }

    private func send(_ target: Target, describe: String) {
        if link.isBusy {
            eventLog.append("Request in progress; move queued behind it")
        }
        let summary = [("pan", target.pan), ("tilt", target.tilt), ("zoom", target.zoom)]
            .compactMap { name, v in v.map { "\(name)=\(format($0))" } }.joined(separator: ", ")
        link.move(target) { [weak self] result in
            guard let self else { return }
            switch result {
            case .success:
                self.eventLog.append("\(describe) accepted: \(summary) (movement may still be in progress)")
                self.nextRefresh = .distantPast
            case .failure(let error):
                if error.linkLost { self.linkLost(error.message); return }
                self.eventLog.append("\(describe) failed: \(error.message)", warning: true)
            }
            self.updateControls()
        }
        updateControls()
    }

    @objc func stopAction(_ sender: Any?) {
        guard state == .connected else { return }
        link.stop { [weak self] result in
            guard let self else { return }
            switch result {
            case .success: self.eventLog.append("Stop accepted")
            case .failure(let error):
                if error.linkLost { self.linkLost(error.message); return }
                self.eventLog.append("Stop failed: \(error.message)", warning: true)
            }
            self.nextRefresh = .distantPast
        }
    }

    @objc func openWebInterface(_ sender: Any?) {
        guard state == .connected, !pendingWebOpen else { return }
        pendingWebOpen = true
        eventLog.append("Locating camera web interface (HTTP first, HTTPS fallback; no credentials sent)…")
        link.webURL { [weak self] result in
            guard let self else { return }
            self.pendingWebOpen = false
            switch result {
            case .success(let (url, notice)):
                self.eventLog.append("Camera web interface: \(url.absoluteString)" + (notice.isEmpty ? "" : " — \(notice)"))
                if !NSWorkspace.shared.open(url) {
                    let alert = NSAlert()
                    alert.messageText = "Open the camera web interface"
                    alert.informativeText = "The browser could not be opened automatically. Open this address:\n\(url.absoluteString)"
                    alert.runModal()
                }
            case .failure(let error):
                if error.linkLost { self.linkLost(error.message); return }
                self.eventLog.append("Camera web interface: \(error.message)", warning: true)
            }
        }
    }

    // MARK: State → views

    private func updateControls() {
        let connected = state == .connected
        connection.setEditable(state == .idle)
        connection.connectButton.title = switch state {
        case .idle: "Connect"
        case .connecting: "Cancel"
        case .connected: "Disconnect"
        case .disconnecting: "Disconnecting…"
        }
        connection.connectButton.isEnabled = state != .disconnecting
        connection.connectButton.keyEquivalent = state == .idle ? "\r" : ""
        for control in [targetPan, targetTilt, targetZoom, moveButton, stopButton, webButton] as [NSControl] {
            control.isEnabled = connected
        }
        startButton.isEnabled = connected && !(starting?.isEmpty ?? true)
        plane.enabled = connected
        for meter in [panMeter, tiltMeter, zoomMeter] { meter.enabled = connected }
        zoomMeter.integer = info?.zoomInteger == true

        plane.panRange = info?.pan
        plane.tiltRange = info?.tilt
        plane.current = connected ? current : nil
        plane.starting = connected ? starting : nil
        panMeter.range = info?.pan
        tiltMeter.range = info?.tilt
        zoomMeter.range = info?.zoom
        panMeter.value = current?.pan?.value
        tiltMeter.value = current?.tilt?.value
        zoomMeter.value = current?.zoom?.value
        panMeter.start = starting?.pan?.value
        tiltMeter.start = starting?.tilt?.value
        zoomMeter.start = starting?.zoom?.value
        updateTargetMarkers()

        func rangeText(_ r: AxisRange?) -> String { r.map { "\(format($0.min)) … \(format($0.max))" } ?? "range not advertised" }
        panReadout.set(connected ? current?.pan?.text : nil, detail: connected ? rangeText(info?.pan) : "")
        tiltReadout.set(connected ? current?.tilt?.text : nil, detail: connected ? rangeText(info?.tilt) : "")
        zoomReadout.set(connected ? current?.zoom?.text : nil,
                        detail: connected ? rangeText(info?.zoom) + (info?.zoomInteger == true ? " · integer" : "") : "")
        statusReadout.set(connected ? current?.moveStatus : nil,
                          detail: connected && current != nil && current?.moveStatus == nil ? "not reported by camera" : "")
        startReadout.stringValue = "Starting: " + (connected && starting != nil ? Self.describe(starting!) : "—")

        if let info, connected {
            headerTitle.stringValue = "\(settings.cameraHost) · \(info.name)"
            headerUnits.stringValue = info.units
        } else {
            headerTitle.stringValue = state == .connecting ? "Connecting to \(settings.cameraHost)…" : "No camera link"
            headerUnits.stringValue = "Choose a route and vendor, enter the camera details, then Connect."
        }

        let chipText: String = switch state {
        case .idle: "OFFLINE"
        case .connecting: "LINKING"
        case .disconnecting: "CLOSING"
        case .connected: link.isBusy ? "REQUEST" : paused ? "READS PAUSED" : current?.isEmpty == false ? "POSITION OK" : "NO POSITION"
        }
        let chipLevel: StatusChip.Level = switch state {
        case .connected: paused || current?.isEmpty != false ? .warn : .good
        case .connecting, .disconnecting: .warn
        case .idle: .muted
        }
        statusChip.set(text: chipText, level: chipLevel)

        let route = settings.route == .ssh ? "SSH \(settings.sshUser)@\(settings.sshHost):\(settings.sshPort)" : "LOCAL"
        footer.stringValue = connected
            ? "\(route)  ·  \(settings.cameraHost):\(settings.cameraPort)  ·  \(paused ? "auto-refresh PAUSED (⌘R resumes)" : "2 s read-only refresh")  ·  \(link.isBusy ? "busy" : "ready")"
            : "\(route)  ·  credentials stay in memory only  ·  Return connects"
        window?.isDocumentEdited = false
    }

    // MARK: CameraLinkDelegate

    func cameraLink(_ link: CameraLink, log line: String) { eventLog.append(line) }

    func cameraLink(_ link: CameraLink, prompt message: String, secret: Bool) -> String? {
        let alert = NSAlert()
        alert.messageText = "SSH needs your input"
        alert.informativeText = message
        alert.addButton(withTitle: "OK")
        alert.addButton(withTitle: "Cancel")
        let field: NSTextField = secret ? NSSecureTextField(frame: NSRect(x: 0, y: 0, width: 280, height: 24))
            : NSTextField(frame: NSRect(x: 0, y: 0, width: 280, height: 24))
        alert.accessoryView = field
        alert.window.initialFirstResponder = field
        NSApp.activate()
        let answer = alert.runModal() == .alertFirstButtonReturn ? field.stringValue : nil
        field.stringValue = ""
        return answer
    }

    func cameraLink(_ link: CameraLink, confirm title: String, message: String) -> Bool {
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = title
        alert.informativeText = message
        alert.addButton(withTitle: "Continue")
        alert.addButton(withTitle: "Cancel")
        NSApp.activate()
        return alert.runModal() == .alertFirstButtonReturn
    }

    // MARK: Window

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        guard state == .connected || state == .connecting else { return true }
        disconnect { NSApp.terminate(nil) }
        return false
    }

    var canAct: Bool { state == .connected }
    var stateValue: State { state }

    // MARK: UI test hook

    /// PTZ_SNAPSHOT_PATH=/path.png renders the window there after a scripted
    /// connect + move (password from PTZ_PASSWORD). Used with tools/mock-camera.py.
    func runSnapshotScript(to path: String) {
        if let raw = ProcessInfo.processInfo.environment["PTZ_THEME"], let t = PhosphorTheme(rawValue: raw) { theme = t }
        connect()
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) {
            self.targetPan.stringValue = "0.55"
            self.targetTilt.stringValue = "0.22"
            self.targetZoom.stringValue = "0.4"
            self.sendMove()
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 4.2) {
            self.targetPan.stringValue = "-0.3"
            self.targetTilt.stringValue = "0.6"
            self.zoomMeter.onAim?(0.75)  // as if dragged along the zoom meter
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 7) {
            guard let view = self.window?.contentView, let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { return }
            view.cacheDisplay(in: view.bounds, to: rep)
            try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: path))
            NSApp.terminate(nil)
        }
    }
}

extension ConsoleWindowController: NSTextFieldDelegate {
    func controlTextDidChange(_ obj: Notification) { updateTargetMarkers() }
}

/// Solid background following the theme.
final class ThemedBackgroundView: NSView {
    var color: NSColor = .windowBackgroundColor { didSet { needsDisplay = true } }
    override var wantsUpdateLayer: Bool { true }
    override func updateLayer() { layer?.backgroundColor = color.cgColor }
    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
    }
    required init?(coder: NSCoder) { fatalError() }
}

/// Small status lamp + label, e.g. "● POSITION OK".
final class StatusChip: NSView {
    enum Level { case good, warn, muted }
    private let label = NSTextField(labelWithString: "OFFLINE")
    private var level = Level.muted
    var theme = PhosphorTheme.system { didSet { refresh() } }

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        label.font = Fonts.mono(11, .bold)
        label.translatesAutoresizingMaskIntoConstraints = false
        addSubview(label)
        NSLayoutConstraint.activate([
            label.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 10),
            label.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -10),
            label.topAnchor.constraint(equalTo: topAnchor, constant: 4),
            label.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -4),
        ])
    }

    required init?(coder: NSCoder) { fatalError() }

    func set(text: String, level: Level) {
        label.stringValue = "● " + text
        self.level = level
        refresh()
    }

    private func refresh() {
        let color: NSColor = switch level {
        case .good: theme.good
        case .warn: theme == .system ? .systemOrange : theme.primary.withAlphaComponent(0.75)
        case .muted: theme.secondary
        }
        label.textColor = color
        layer?.borderColor = color.withAlphaComponent(0.5).cgColor
        layer?.borderWidth = 1
        layer?.cornerRadius = 11
    }
}

/// Monospaced, timestamped event buffer.
final class EventLogView: NSScrollView {
    private let textView = NSTextView()
    private var lines = 0
    var theme = PhosphorTheme.system {
        didSet {
            backgroundColor = theme.panel
            textView.backgroundColor = theme.panel
            textView.textColor = theme.text
        }
    }

    override init(frame: NSRect) {
        super.init(frame: frame)
        hasVerticalScroller = true
        borderType = .noBorder
        drawsBackground = true
        textView.isEditable = false
        textView.isSelectable = true
        textView.font = Fonts.mono(11)
        textView.textContainerInset = NSSize(width: 2, height: 2)
        textView.isVerticallyResizable = true
        textView.autoresizingMask = [.width]
        textView.textContainer?.widthTracksTextView = true
        documentView = textView
    }

    required init?(coder: NSCoder) { fatalError() }

    private static let clock: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss"
        return f
    }()

    func append(_ line: String, warning: Bool = false) {
        guard let storage = textView.textStorage else { return }
        let text = "\(Self.clock.string(from: Date()))  \(line)\n"
        let attributes: [NSAttributedString.Key: Any] = [
            .font: Fonts.mono(11),
            .foregroundColor: warning ? theme.warning : theme.text,
        ]
        let atBottom = (contentView.bounds.maxY >= (documentView?.bounds.maxY ?? 0) - 4)
        storage.append(NSAttributedString(string: text, attributes: attributes))
        lines += 1
        if lines > 500, let first = storage.string.range(of: "\n") {
            storage.deleteCharacters(in: NSRange(storage.string.startIndex...first.lowerBound, in: storage.string))
            lines -= 1
        }
        if atBottom { textView.scrollToEndOfDocument(nil) }
    }
}
