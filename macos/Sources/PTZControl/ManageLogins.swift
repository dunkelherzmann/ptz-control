import AppKit

/// Manage Logins: the saved SSH logins and the saved camera logins, each in its own
/// list, with New, Edit and Delete, the Administrator Password and Lock/Unlock.
@MainActor final class ManageLoginsWindowController: NSWindowController, NSTableViewDataSource, NSTableViewDelegate {
    static let shared = ManageLoginsWindowController()

    private let storeControl = NSSegmentedControl(labels: ["SSH Logins", "Camera Logins"], trackingMode: .selectOne,
                                                  target: nil, action: nil)
    private let table = LoginTable()
    private let newButton = NSButton(title: "New…", target: nil, action: nil)
    private let editButton = NSButton(title: "Edit…", target: nil, action: nil)
    private let deleteButton = NSButton(title: "Delete", target: nil, action: nil)
    private let adminButton = NSButton(title: "Create Administrator Password…", target: nil, action: nil)
    private let lockButton = NSButton(title: "Lock", target: nil, action: nil)
    private let doneButton = NSButton(title: "Done", target: nil, action: nil)
    private let status = NSTextField(labelWithString: "")
    private var logins: [LoginVault.Login] = []
    private var store: LoginVault.Store { storeControl.selectedSegment == 1 ? .camera : .ssh }
    private let vault = LoginVault.shared

    private init() {
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 640, height: 400),
                              styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        window.title = "Manage Logins"
        window.minSize = NSSize(width: 560, height: 300)
        window.tabbingMode = .disallowed
        window.isReleasedWhenClosed = false
        super.init(window: window)
        build()
        NotificationCenter.default.addObserver(forName: LoginVault.didChange, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.reload() }
        }
    }

    required init?(coder: NSCoder) { fatalError() }

    /// Opens the window, asking for the Administrator Password first when logins are locked.
    func present() {
        guard vault.state != .unavailable else {
            VaultPrompts.showError("Saved logins are unavailable", vault.openError ?? "")
            return
        }
        if vault.state == .locked { _ = VaultPrompts.unlock() }
        reload()
        if window?.isVisible != true { window?.center() }
        showWindow(nil)
        window?.makeFirstResponder(table)
    }

    private func build() {
        guard let content = window?.contentView else { return }
        storeControl.selectedSegment = 0
        storeControl.target = self
        storeControl.action = #selector(storeChanged)
        storeControl.toolTip = "SSH Logins: saved SSH hosts. Camera Logins: saved camera accounts. The two are kept apart, so any SSH host can be used with any camera login."
        storeControl.setToolTip("Saved SSH host logins", forSegment: 0)
        storeControl.setToolTip("Saved camera logins", forSegment: 1)

        for (title, width) in [("Host", 200.0), ("Port", 60.0), ("Username", 130.0), ("Password", 90.0), ("Key file", 160.0)] {
            let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(title))
            column.title = title == "Host" ? "Host or address" : title
            column.width = width
            column.minWidth = 50
            table.addTableColumn(column)
        }
        table.dataSource = self
        table.delegate = self
        table.usesAlternatingRowBackgroundColors = true
        table.allowsMultipleSelection = false
        table.doubleAction = #selector(edit)
        table.target = self
        table.onDelete = { [weak self] in self?.delete() }
        table.setAccessibilityLabel("Saved logins")
        let scroll = NSScrollView()
        scroll.documentView = table
        scroll.hasVerticalScroller = true
        scroll.borderType = .bezelBorder

        newButton.toolTip = "New login — Add a saved login to this list."
        editButton.toolTip = "Edit login — Change the selected login (or double-click it)."
        deleteButton.toolTip = "Delete login — Remove the selected login after a confirmation (⌫)."
        adminButton.toolTip = "Administrator Password — Encrypts every saved SSH and camera login. Change it, or leave the new password blank to keep logins as plain text."
        lockButton.toolTip = "Lock or unlock the saved logins with the Administrator Password."
        doneButton.toolTip = "Close Manage Logins (Esc)."
        newButton.action = #selector(newLogin)
        editButton.action = #selector(edit)
        deleteButton.action = #selector(delete)
        adminButton.action = #selector(admin)
        lockButton.action = #selector(lockOrUnlock)
        doneButton.action = #selector(done)
        doneButton.keyEquivalent = "\u{1b}"
        for b in [newButton, editButton, deleteButton, adminButton, lockButton, doneButton] {
            b.target = self
            b.bezelStyle = .push
        }
        status.font = .systemFont(ofSize: 11)
        status.textColor = .secondaryLabelColor
        status.lineBreakMode = .byTruncatingTail

        let top = NSStackView(views: [storeControl, NSView(), newButton, editButton, deleteButton])
        let bottom = NSStackView(views: [lockButton, adminButton, status, NSView(), doneButton])
        status.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        let stack = NSStackView(views: [top, scroll, bottom])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 10
        stack.edgeInsets = NSEdgeInsets(top: 14, left: 14, bottom: 14, right: 14)
        stack.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: content.topAnchor),
            stack.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            stack.bottomAnchor.constraint(equalTo: content.bottomAnchor),
            top.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -28),
            scroll.widthAnchor.constraint(equalTo: top.widthAnchor),
            bottom.widthAnchor.constraint(equalTo: top.widthAnchor),
        ])
    }

    private func reload() {
        let selected = table.selectedRow
        logins = vault.logins(store)
        table.tableColumns.last?.isHidden = store == .camera
        table.reloadData()
        if selected >= 0, selected < logins.count { table.selectRowIndexes([selected], byExtendingSelection: false) }
        let state = vault.state
        adminButton.title = state == .unlocked || state == .locked ? "Change Administrator Password…" : "Create Administrator Password…"
        lockButton.title = state == .locked ? "Unlock…" : "Lock"
        lockButton.isEnabled = state == .locked || state == .unlocked
        lockButton.setAccessibilityLabel(state == .locked ? "Unlock Saved Logins" : "Lock Saved Logins")
        status.stringValue = switch state {
        case .locked: "Locked."
        case .unlocked: "Encrypted with the Administrator Password."
        case .plain: "Stored as plain text."
        case .new: "No logins saved yet."
        case .unavailable: vault.openError ?? "Saved logins are unavailable."
        }
        let readable = vault.isReadable || state == .new
        newButton.isEnabled = readable
        editButton.isEnabled = readable && table.selectedRow >= 0
        deleteButton.isEnabled = readable && table.selectedRow >= 0
    }

    // MARK: Table

    func numberOfRows(in tableView: NSTableView) -> Int { logins.count }

    func tableView(_ tableView: NSTableView, viewFor column: NSTableColumn?, row: Int) -> NSView? {
        let l = logins[row]
        let text: String = switch column?.identifier.rawValue {
        case "Host": l.host
        case "Port": String(l.port)
        case "Username": l.user
        case "Password": l.password.isEmpty ? "—" : "••••••"
        default: l.keyFile.isEmpty ? "—" : l.keyFile
        }
        let cell = NSTextField(labelWithString: text)
        cell.font = column?.identifier.rawValue == "Password" ? .systemFont(ofSize: 12) : Fonts.mono(12)
        cell.lineBreakMode = .byTruncatingTail
        return cell
    }

    func tableViewSelectionDidChange(_ notification: Notification) { reload() }

    // MARK: Actions

    @objc private func storeChanged() {
        table.deselectAll(nil)
        reload()
    }

    @objc private func newLogin() {
        guard VaultPrompts.ensureWritable() else { return }
        var blank = LoginVault.Login()
        blank.port = store == .ssh ? 22 : 80
        if let login = LoginForm.run(blank, store: store, title: store == .ssh ? "New SSH Login" : "New Camera Login") {
            save(login, replacing: nil)
        }
    }

    @objc private func edit() {
        let row = table.clickedRow >= 0 ? table.clickedRow : table.selectedRow
        guard row >= 0, row < logins.count, VaultPrompts.ensureWritable() else { return }
        if let login = LoginForm.run(logins[row], store: store, title: store == .ssh ? "Edit SSH Login" : "Edit Camera Login") {
            save(login, replacing: row)
        }
    }

    private func save(_ login: LoginVault.Login, replacing row: Int?) {
        do {
            let index = try vault.save(login, in: store, replacing: row)
            table.selectRowIndexes([index], byExtendingSelection: false)
            reload()
        } catch {
            VaultPrompts.showError("Can't save the login", error.localizedDescription)
        }
    }

    @objc private func delete() {
        let row = table.selectedRow
        guard row >= 0, row < logins.count, vault.isReadable else { return }
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = "Delete the login \(logins[row].label)?"
        alert.informativeText = "This removes it from the saved \(store == .ssh ? "SSH" : "camera") logins."
        alert.addButton(withTitle: "Cancel")
        alert.addButton(withTitle: "Delete")
        guard alert.runModal() == .alertSecondButtonReturn else { return }
        do { try vault.remove(row, from: store) } catch {
            VaultPrompts.showError("Can't delete the login", error.localizedDescription)
        }
    }

    @objc private func admin() {
        VaultPrompts.changeAdminPassword()
        reload()
    }

    @objc private func lockOrUnlock() {
        if vault.state == .locked { _ = VaultPrompts.unlock() } else { vault.lock() }
    }

    @objc private func done() { window?.performClose(nil) }

    /// Renders the window's contents for one store to a PNG, without showing it (test hook).
    func snapshot(store: LoginVault.Store, to path: String) {
        storeControl.selectedSegment = store.rawValue
        reload()
        guard let window, let view = window.contentView else { return }
        // The window draws its own background on screen; the picture needs it painted in.
        view.wantsLayer = true
        view.effectiveAppearance.performAsCurrentDrawingAppearance {
            view.layer?.backgroundColor = NSColor.windowBackgroundColor.cgColor
        }
        view.layoutSubtreeIfNeeded()
        guard let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { return }
        view.cacheDisplay(in: view.bounds, to: rep)
        try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: path))
        // And the Edit form for the first login, as it appears inside its dialog.
        guard let first = logins.first else { return }
        let form = LoginFormView(first, store: store)
        let holder = NSView(frame: NSRect(x: 0, y: 0, width: form.frame.width + 24, height: form.frame.height + 24))
        holder.appearance = view.effectiveAppearance
        holder.wantsLayer = true
        view.effectiveAppearance.performAsCurrentDrawingAppearance {
            holder.layer?.backgroundColor = NSColor.windowBackgroundColor.cgColor
        }
        form.setFrameOrigin(NSPoint(x: 12, y: 12))
        holder.addSubview(form)
        guard let formRep = holder.bitmapImageRepForCachingDisplay(in: holder.bounds) else { return }
        holder.cacheDisplay(in: holder.bounds, to: formRep)
        try? formRep.representation(using: .png, properties: [:])?
            .write(to: URL(fileURLWithPath: path.replacingOccurrences(of: ".png", with: "-form.png")))
    }
}

/// The list of logins; Delete or Backspace removes the selected one.
final class LoginTable: NSTableView {
    var onDelete: (() -> Void)?

    override func keyDown(with event: NSEvent) {
        if event.keyCode == 51 || event.keyCode == 117 { onDelete?() } else { super.keyDown(with: event) }
    }
}

/// The New and Edit form: host or address, port, username, password (with Show password)
/// and, for SSH logins, the key file.
@MainActor final class LoginFormView: NSGridView {
    let host = NSTextField()
    let port = NSTextField()
    let user = NSTextField()
    let secureField = NSSecureTextField()
    let plainField = NSTextField()
    let show = NSButton(checkboxWithTitle: "Show password", target: nil, action: nil)
    let key = NSTextField()
    private let toggle: ShowPasswordToggle
    let store: LoginVault.Store

    init(_ login: LoginVault.Login, store: LoginVault.Store) {
        self.store = store
        toggle = ShowPasswordToggle(hidden: secureField, shown: plainField)
        super.init(frame: .zero)
        host.stringValue = login.host
        port.stringValue = String(login.port)
        user.stringValue = login.user
        secureField.stringValue = login.password
        plainField.stringValue = login.password
        key.stringValue = login.keyFile
        host.placeholderString = store == .ssh ? "jump.example.com" : "192.168.1.100"
        user.placeholderString = store == .ssh ? "username" : "admin"
        secureField.placeholderString = store == .ssh ? "blank for keys / agent" : "camera password"
        plainField.placeholderString = secureField.placeholderString
        key.placeholderString = "optional, e.g. ~/.ssh/id_ed25519"
        show.toolTip = "Show password — Shows the saved password as text while this form is open."
        show.target = toggle
        show.action = #selector(ShowPasswordToggle.flip(_:))
        for (field, label) in [(host, store == .ssh ? "Host" : "Address"), (port, "Port"), (user, "Username"),
                               (secureField, "Password"), (plainField, "Password"), (key, "Key file")] {
            field.font = Fonts.mono(12)
            field.usesSingleLineMode = true
            field.setAccessibilityLabel(label)
            field.translatesAutoresizingMaskIntoConstraints = false
            field.widthAnchor.constraint(equalToConstant: 250).isActive = true
        }
        plainField.isHidden = true
        let password = NSStackView(views: [secureField, plainField])

        func label(_ text: String) -> NSTextField {
            let l = NSTextField(labelWithString: text)
            l.alignment = .right
            return l
        }
        addRow(with: [label(store == .ssh ? "Host" : "Address"), host])
        addRow(with: [label("Port"), port])
        addRow(with: [label("Username"), user])
        addRow(with: [label("Password"), password])
        addRow(with: [NSGridCell.emptyContentView, show])
        if store == .ssh { addRow(with: [label("Key file"), key]) }
        rowSpacing = 8
        columnSpacing = 8
        column(at: 0).xPlacement = .trailing
        host.nextKeyView = port
        port.nextKeyView = user
        user.nextKeyView = secureField
        secureField.nextKeyView = store == .ssh ? key : host
        frame = NSRect(origin: .zero, size: fittingSize)
    }

    required init?(coder: NSCoder) { fatalError() }

    var password: String {
        get { toggle.sync(); return secureField.stringValue }
        set { secureField.stringValue = newValue; plainField.stringValue = newValue }
    }

    /// The login as entered, or the reason it can't be saved.
    func login() -> Result<LoginVault.Login, LinkError> {
        var result = LoginVault.Login()
        result.host = host.stringValue.trimmingCharacters(in: .whitespaces)
        result.user = user.stringValue.trimmingCharacters(in: .whitespaces)
        result.password = password
        result.keyFile = store == .ssh ? key.stringValue.trimmingCharacters(in: .whitespaces) : ""
        guard let p = Int(port.stringValue.trimmingCharacters(in: .whitespaces)), (1...65535).contains(p) else {
            return .failure(LinkError(message: "The port must be an integer between 1 and 65535."))
        }
        result.port = p
        if result.host.isEmpty || result.user.isEmpty {
            return .failure(LinkError(message: "\(store == .ssh ? "Host" : "Address") and username are required."))
        }
        return .success(result)
    }
}

@MainActor enum LoginForm {
    static func run(_ login: LoginVault.Login, store: LoginVault.Store, title: String) -> LoginVault.Login? {
        let form = LoginFormView(login, store: store)
        defer { form.password = "" }
        var note = store == .ssh ? "The password may stay blank for keys or ssh-agent." : ""
        while true {
            let alert = NSAlert()
            alert.messageText = title
            alert.informativeText = note
            alert.addButton(withTitle: "Save")
            alert.addButton(withTitle: "Cancel")
            alert.accessoryView = form
            alert.window.initialFirstResponder = form.host
            NSApp.activate()
            guard alert.runModal() == .alertFirstButtonReturn else { return nil }
            switch form.login() {
            case .success(let result): return result
            case .failure(let error): note = error.message
            }
        }
    }
}

/// Swaps the password between a secure field and a plain one, keeping their text the same.
@MainActor final class ShowPasswordToggle: NSObject {
    let hidden: NSSecureTextField
    let shown: NSTextField
    private var showing = false

    init(hidden: NSSecureTextField, shown: NSTextField) {
        self.hidden = hidden
        self.shown = shown
    }

    @objc func flip(_ sender: NSButton) {
        sync()
        showing = sender.state == .on
        hidden.isHidden = showing
        shown.isHidden = !showing
        hidden.window?.makeFirstResponder(showing ? shown : hidden)
    }

    /// Copies the visible field's text into the other one.
    func sync() {
        if showing { hidden.stringValue = shown.stringValue } else { shown.stringValue = hidden.stringValue }
    }
}
