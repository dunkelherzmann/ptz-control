import AppKit
import PTZCore

/// Sidebar form: route, vendor, SSH host and camera credentials. Passwords are
/// held in the fields, and written only to the login vault when Save login is ticked.
final class ConnectionPanel: NSView, Themed, NSTextFieldDelegate {
    let routeControl = NSSegmentedControl(labels: ["Local", "SSH Host"], trackingMode: .selectOne, target: nil, action: nil)
    let vendorControl = NSSegmentedControl(labels: ["Bosch/Milesight", "Axis"], trackingMode: .selectOne, target: nil, action: nil)
    let sshHost = NSTextField()
    let sshUser = NSTextField()
    let sshPassword = NSSecureTextField()
    let sshPort = NSTextField()
    let sshKey = NSTextField()
    let cameraHost = NSTextField()
    let cameraPort = NSTextField()
    let cameraUser = NSTextField()
    let cameraPassword = NSSecureTextField()
    let httpsPort = NSTextField()
    let profile = NSTextField()
    let timeout = NSTextField()
    let sshLogin = NSPopUpButton(frame: .zero, pullsDown: false)
    let cameraLogin = NSPopUpButton(frame: .zero, pullsDown: false)
    let saveSSH = NSButton(checkboxWithTitle: "Save login", target: nil, action: nil)
    let saveCamera = NSButton(checkboxWithTitle: "Save login", target: nil, action: nil)
    let connectButton = NSButton(title: "Connect", target: nil, action: nil)
    let message = NSTextField(wrappingLabelWithString: "")

    private var sshRows: [NSView] = []
    private var captions: [NSTextField] = []
    private let advancedToggle = NSButton(title: "Advanced", target: nil, action: nil)
    private var advancedRows: [NSView] = []
    private let vendorNote = NSTextField(wrappingLabelWithString: "")
    private var theme = PhosphorTheme.system
    private var messageIsError = false

    var onConnect: (() -> Void)?
    var onManageLogins: ((LoginVault.Store) -> Void)?

    init() {
        super.init(frame: .zero)
        routeControl.target = self
        routeControl.action = #selector(routeChanged)
        vendorControl.target = self
        vendorControl.action = #selector(vendorChanged)
        connectButton.target = self
        connectButton.action = #selector(connect)
        connectButton.bezelStyle = .push
        connectButton.keyEquivalent = "\r"
        connectButton.controlSize = .large
        advancedToggle.setButtonType(.pushOnPushOff)
        advancedToggle.bezelStyle = .disclosure
        advancedToggle.title = ""
        advancedToggle.target = self
        advancedToggle.action = #selector(toggleAdvanced)

        sshHost.placeholderString = "jump.example.com"
        sshUser.placeholderString = "username"
        sshPassword.placeholderString = "blank for keys / agent"
        sshKey.placeholderString = "optional, e.g. ~/.ssh/id_ed25519"
        cameraHost.placeholderString = "192.168.1.100"
        cameraPassword.placeholderString = ProcessInfo.processInfo.environment["PTZ_PASSWORD"] == nil ? "required" : "from PTZ_PASSWORD"
        if let env = ProcessInfo.processInfo.environment["PTZ_PASSWORD"] { cameraPassword.stringValue = env }
        for field in [sshHost, sshUser, sshPassword, sshPort, sshKey, cameraHost, cameraPort, cameraUser, cameraPassword,
                      httpsPort, profile, timeout] {
            field.font = Fonts.mono(12)
            field.lineBreakMode = .byTruncatingTail
            field.usesSingleLineMode = true
            field.cell?.isScrollable = true
            field.target = self
            field.action = #selector(fieldAction(_:))
        }
        for field in [sshHost, sshUser, cameraHost, cameraUser] {
            field.contentType = nil
            field.isAutomaticTextCompletionEnabled = false
        }
        for field in [sshUser, sshPassword, cameraUser, cameraPassword] { field.delegate = self }
        for popup in [sshLogin, cameraLogin] {
            popup.autoenablesItems = false
            popup.target = self
            popup.action = #selector(loginChosen(_:))
            popup.font = .systemFont(ofSize: 12)
        }
        sshLogin.toolTip = "SSH login — Fill the SSH username and password from a saved SSH login, or open Manage Logins (⇧⌘L)."
        cameraLogin.toolTip = "Camera login — Fill the camera username and password from a saved camera login, or open Manage Logins (⇧⌘L)."
        sshLogin.setAccessibilityLabel("Saved SSH login")
        cameraLogin.setAccessibilityLabel("Saved camera login")
        saveSSH.toolTip = "Save login — Once connected, save this SSH username and password to the saved SSH logins."
        saveCamera.toolTip = "Save login — Once connected, save this camera username and password to the saved camera logins."
        saveSSH.setAccessibilityLabel("Save this SSH login")
        saveCamera.setAccessibilityLabel("Save this camera login")
        for box in [saveSSH, saveCamera] { box.font = .systemFont(ofSize: 12) }

        let stack = NSStackView()
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 6
        stack.translatesAutoresizingMaskIntoConstraints = false

        func caption(_ text: String) -> NSTextField {
            let label = NSTextField(labelWithString: text.uppercased())
            label.font = Fonts.caption()
            captions.append(label)
            return label
        }
        func row(_ title: String, _ field: NSView) -> NSView {
            let label = NSTextField(labelWithString: title)
            label.font = .systemFont(ofSize: 12)
            label.alignment = .right
            label.translatesAutoresizingMaskIntoConstraints = false
            label.widthAnchor.constraint(equalToConstant: 82).isActive = true
            let h = NSStackView(views: [label, field])
            h.orientation = .horizontal
            h.spacing = 8
            return h
        }
        func spacer(_ height: CGFloat) -> NSView {
            let v = NSView()
            v.translatesAutoresizingMaskIntoConstraints = false
            v.heightAnchor.constraint(equalToConstant: height).isActive = true
            return v
        }

        let routeCaption = caption("Route")
        let vendorCaption = caption("Camera vendor")
        let sshCaption = caption("SSH host")
        sshRows = [sshCaption, row("Login", sshLogin), row("Host", sshHost), row("Username", sshUser),
                   row("Password", sshPassword), row("Port", sshPort), row("Key file", sshKey), row("", saveSSH)]
        let advancedRow = NSStackView(views: [advancedToggle, caption("Advanced")])
        advancedRow.spacing = 2
        advancedRows = [row("HTTPS port", httpsPort), row("Profile", profile), row("Timeout (s)", timeout)]

        let views: [NSView] = [routeCaption, routeControl, spacer(4), vendorCaption, vendorControl, vendorNote, spacer(4)]
            + sshRows + [spacer(4), caption("Camera"), row("Login", cameraLogin), row("Address", cameraHost),
                         row("HTTP port", cameraPort), row("Username", cameraUser), row("Password", cameraPassword),
                         row("", saveCamera), spacer(2), advancedRow]
            + advancedRows + [spacer(8), connectButton, message]
        for view in views { stack.addArrangedSubview(view) }
        for view in [routeControl, vendorControl, connectButton, message, vendorNote] {
            view.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        }
        for view in sshRows + advancedRows + [cameraLogin, cameraHost, cameraPort, cameraUser, cameraPassword, saveCamera].map({ $0.superview! }) {
            view.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        }
        vendorNote.font = .systemFont(ofSize: 11)
        message.font = .systemFont(ofSize: 11)
        message.maximumNumberOfLines = 6

        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: topAnchor),
            stack.leadingAnchor.constraint(equalTo: leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor),
            stack.bottomAnchor.constraint(lessThanOrEqualTo: bottomAnchor),
        ])
        advancedRows.forEach { $0.isHidden = true }
        reloadLogins()
        NotificationCenter.default.addObserver(forName: LoginVault.didChange, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.reloadLogins() }
        }
    }

    required init?(coder: NSCoder) { fatalError() }

    func load(_ s: ConnectionSettings) {
        routeControl.selectedSegment = s.route.rawValue
        vendorControl.selectedSegment = s.vendor.rawValue
        sshHost.stringValue = s.sshHost
        sshUser.stringValue = s.sshUser
        sshPort.stringValue = String(s.sshPort)
        sshKey.stringValue = s.sshIdentity
        cameraHost.stringValue = s.cameraHost
        cameraPort.stringValue = String(s.cameraPort)
        cameraUser.stringValue = s.cameraUser
        httpsPort.stringValue = String(s.httpsPort)
        profile.stringValue = String(s.profile)
        timeout.stringValue = format(s.timeout)
        routeChanged()
        vendorChanged()
    }

    /// Validates the non-secret fields; throws a message for the operator.
    func settings() throws -> ConnectionSettings {
        func port(_ field: NSTextField, _ name: String) throws -> Int {
            guard let v = Int(field.stringValue.trimmingCharacters(in: .whitespaces)), (1...65535).contains(v) else {
                throw LinkError(message: "\(name) must be an integer between 1 and 65535")
            }
            return v
        }
        var s = ConnectionSettings()
        s.route = routeControl.selectedSegment == 1 ? .ssh : .local
        s.vendor = vendorControl.selectedSegment == 1 ? .axis : .onvif
        s.cameraHost = cameraHost.stringValue.trimmingCharacters(in: .whitespaces)
        s.cameraUser = cameraUser.stringValue.trimmingCharacters(in: .whitespaces)
        if s.cameraHost.isEmpty { throw LinkError(message: "Camera address is required") }
        if s.cameraUser.isEmpty { throw LinkError(message: "Camera username is required") }
        s.cameraPort = try port(cameraPort, "Camera HTTP port")
        s.httpsPort = try port(httpsPort, "HTTPS port")
        guard let p = Int(profile.stringValue.trimmingCharacters(in: .whitespaces)), p >= 0 else {
            throw LinkError(message: "Profile index must be a nonnegative integer")
        }
        s.profile = p
        var t = 0.0
        guard ptz_parse(timeout.stringValue, &t), t > 0, t <= 300 else {
            throw LinkError(message: "Timeout must be a number of seconds between 0 and 300")
        }
        s.timeout = t
        if s.route == .ssh {
            s.sshHost = sshHost.stringValue.trimmingCharacters(in: .whitespaces)
            s.sshUser = sshUser.stringValue.trimmingCharacters(in: .whitespaces)
            if s.sshHost.isEmpty { throw LinkError(message: "SSH host is required") }
            if s.sshUser.isEmpty { throw LinkError(message: "SSH username is required") }
            s.sshPort = try port(sshPort, "SSH port")
            s.sshIdentity = sshKey.stringValue.trimmingCharacters(in: .whitespaces)
        } else {
            s.sshHost = sshHost.stringValue
            s.sshUser = sshUser.stringValue
            s.sshPort = Int(sshPort.stringValue) ?? 22
            s.sshIdentity = sshKey.stringValue
        }
        return s
    }

    func setEditable(_ editable: Bool) {
        for control in [routeControl, vendorControl, sshLogin, sshHost, sshUser, sshPassword, sshPort, sshKey, saveSSH,
                        cameraLogin, cameraHost, cameraPort, cameraUser, cameraPassword, saveCamera, httpsPort, profile,
                        timeout] as [NSControl] {
            control.isEnabled = editable
        }
    }

    func show(message text: String, error: Bool) {
        message.stringValue = text
        messageIsError = error
        apply(theme: theme)
    }

    func apply(theme: PhosphorTheme) {
        self.theme = theme
        captions.forEach { $0.textColor = theme.secondary }
        vendorNote.textColor = theme.secondary
        message.textColor = messageIsError ? theme.warning : theme.secondary
    }

    @objc private func routeChanged() {
        let ssh = routeControl.selectedSegment == 1
        sshRows.forEach { $0.isHidden = !ssh }
    }

    @objc private func vendorChanged() {
        vendorNote.stringValue = vendorControl.selectedSegment == 1
            ? "Native VAPIX with HTTP Digest. Pan/tilt in degrees, integer zoom."
            : "ONVIF absolute coordinates with camera ONVIF credentials."
    }

    @objc private func toggleAdvanced() {
        advancedRows.forEach { $0.isHidden = advancedToggle.state != .on }
    }

    @objc private func connect() { onConnect?() }

    // MARK: Saved logins

    private enum LoginItem: Int { case typed = -1, manage = -2, unlock = -3 }

    /// Lists the saved logins in both pop-ups, keeping the chosen login where it still exists.
    func reloadLogins() {
        fill(sshLogin, .ssh)
        fill(cameraLogin, .camera)
    }

    private func fill(_ popup: NSPopUpButton, _ store: LoginVault.Store) {
        let previous = popup.selectedTag() >= 0 ? popup.titleOfSelectedItem : nil
        let vault = LoginVault.shared
        popup.removeAllItems()
        popup.addItem(withTitle: "Type a login")
        popup.lastItem?.tag = LoginItem.typed.rawValue
        switch vault.state {
        case .unavailable:
            popup.addItem(withTitle: "Saved logins unavailable")
            popup.lastItem?.isEnabled = false
            popup.lastItem?.toolTip = vault.openError
        case .locked:
            popup.addItem(withTitle: "Unlock Saved Logins…")
            popup.lastItem?.tag = LoginItem.unlock.rawValue
        default:
            for (index, login) in vault.logins(store).enumerated() {
                popup.addItem(withTitle: login.label)
                popup.lastItem?.tag = index
            }
        }
        popup.menu?.addItem(.separator())
        popup.addItem(withTitle: "Manage Logins…")
        popup.lastItem?.tag = LoginItem.manage.rawValue
        popup.lastItem?.isEnabled = vault.state != .unavailable
        if let previous, popup.indexOfItem(withTitle: previous) >= 0 { popup.selectItem(withTitle: previous) } else { popup.selectItem(at: 0) }
    }

    @objc private func loginChosen(_ popup: NSPopUpButton) {
        let store: LoginVault.Store = popup === sshLogin ? .ssh : .camera
        let tag = popup.selectedTag()
        switch tag {
        case LoginItem.manage.rawValue:
            popup.selectItem(at: 0)
            onManageLogins?(store)
        case LoginItem.unlock.rawValue:
            popup.selectItem(at: 0)
            if VaultPrompts.unlock() { popup.performClick(nil) }
        case LoginItem.typed.rawValue: break
        default:
            let logins = LoginVault.shared.logins(store)
            guard tag < logins.count else { return }
            let login = logins[tag]
            if store == .ssh {
                sshUser.stringValue = login.user
                sshPassword.stringValue = login.password
            } else {
                cameraUser.stringValue = login.user
                cameraPassword.stringValue = login.password
            }
        }
    }

    /// The name of the saved login the section's fields still hold, if any.
    func chosenLoginName(_ store: LoginVault.Store) -> String? {
        let popup = store == .ssh ? sshLogin : cameraLogin
        return popup.selectedTag() >= 0 ? popup.titleOfSelectedItem : nil
    }

    /// Chooses a saved login as if picked from the pop-up (used by the snapshot test hook).
    func chooseSavedLogin(_ store: LoginVault.Store, index: Int) {
        let popup = store == .ssh ? sshLogin : cameraLogin
        guard popup.selectItem(withTag: index) else { return }
        loginChosen(popup)
    }

    /// Typing a username or password means it's no longer the saved login.
    func controlTextDidChange(_ obj: Notification) {
        guard let field = obj.object as? NSTextField else { return }
        if field === sshUser || field === sshPassword {
            sshLogin.selectItem(at: 0)
        } else {
            cameraLogin.selectItem(at: 0)
        }
    }

    @objc private func fieldAction(_ sender: NSTextField) {
        // Return in the last field connects; elsewhere it moves on.
        if sender === cameraPassword { onConnect?() } else { window?.selectNextKeyView(sender) }
    }
}

func ptz_parse(_ text: String, _ value: inout Double) -> Bool {
    text.trimmingCharacters(in: .whitespaces).withCString { ptz_parse_number($0, &value) } != 0
}
