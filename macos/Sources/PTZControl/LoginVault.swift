import AppKit
import PTZCore

/// Saved SSH logins and saved camera logins, kept by the core in one file
/// (`~/Library/Application Support/PTZControl/logins.vault`, or `PTZ_VAULT_PATH`
/// for a test copy). Both stores are unlocked by the same Administrator Password.
@MainActor final class LoginVault {
    enum Store: Int { case ssh = 0, camera = 1 }

    /// A saved login: a name that tells it apart, a username and its password.
    struct Login: Equatable {
        var name = ""
        var user = ""
        var password = ""
        var label: String { name }
    }

    enum State { case unavailable, new, plain, locked, unlocked }

    static let shared = LoginVault()
    static let didChange = Notification.Name("PTZLoginVaultDidChange")

    private var handle: OpaquePointer?
    /// Why the file couldn't be opened; saved logins are then off for this session.
    private(set) var openError: String?

    private init() {
        let path: String
        if let override = ProcessInfo.processInfo.environment["PTZ_VAULT_PATH"], !override.isEmpty {
            path = override
        } else {
            let folder = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
                .appendingPathComponent("PTZControl", isDirectory: true)
            try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true,
                                                     attributes: [.posixPermissions: 0o700])
            path = folder.appendingPathComponent("logins.vault").path
        }
        var error = [CChar](repeating: 0, count: Int(PTZ_TEXT))
        handle = ptz_vault_open(path, &error, error.count)
        if handle == nil { openError = String(cString: error) }
    }

    var state: State {
        guard let handle else { return .unavailable }
        switch ptz_vault_get_state(handle) {
        case PTZ_VAULT_PLAIN: return .plain
        case PTZ_VAULT_LOCKED: return .locked
        case PTZ_VAULT_UNLOCKED: return .unlocked
        default: return .new
        }
    }

    var isReadable: Bool { state == .plain || state == .unlocked }

    func logins(_ store: Store) -> [Login] {
        guard let handle else { return [] }
        let s = ptz_store(rawValue: UInt32(store.rawValue))
        return (0..<ptz_vault_count(handle, s)).compactMap { index in
            var l = ptz_login()
            guard ptz_vault_login(handle, s, index, &l) != 0 else { return nil }
            return Login(name: String(cString: l.name), user: String(cString: l.user),
                         password: String(cString: l.password))
        }
    }

    func unlock(_ adminPassword: String) throws {
        try call { ptz_vault_unlock(handle, adminPassword, $0, $1) != 0 }
        changed()
    }

    func lock() {
        ptz_vault_lock(handle)
        changed()
    }

    /// Creates or changes the Administrator Password; an empty one keeps logins as plain text.
    func setAdminPassword(_ adminPassword: String) throws {
        try call { ptz_vault_set_admin_password(handle, adminPassword, $0, $1) != 0 }
        changed()
    }

    /// Saves a login (replacing the one at `index`, or one with the same name) and
    /// returns its index in the sorted list.
    @discardableResult
    func save(_ login: Login, in store: Store, replacing index: Int? = nil) throws -> Int {
        var result = -1
        try call { error, size in
            login.name.withCString { name in
                login.user.withCString { user in
                    login.password.withCString { password in
                        var l = ptz_login(name: name, user: user, password: password)
                        result = ptz_vault_save(handle, ptz_store(rawValue: UInt32(store.rawValue)), index ?? -1, &l,
                                                error, size)
                    }
                }
            }
            return result >= 0
        }
        changed()
        return result
    }

    func remove(_ index: Int, from store: Store) throws {
        try call { ptz_vault_remove(handle, ptz_store(rawValue: UInt32(store.rawValue)), index, $0, $1) != 0 }
        changed()
    }

    private func call(_ body: (UnsafeMutablePointer<CChar>, Int) -> Bool) throws {
        guard handle != nil else { throw LinkError(message: openError ?? "Saved logins are unavailable") }
        var error = [CChar](repeating: 0, count: Int(PTZ_TEXT))
        let ok = error.withUnsafeMutableBufferPointer { body($0.baseAddress!, $0.count) }
        if !ok { throw LinkError(message: String(cString: error)) }
    }

    private func changed() { NotificationCenter.default.post(name: Self.didChange, object: self) }
}

/// The dialogs around the vault: unlocking, the first-use choice and the Administrator Password.
@MainActor enum VaultPrompts {
    /// Makes the saved logins readable, asking for the Administrator Password when they're locked.
    static func ensureReadable() -> Bool {
        let vault = LoginVault.shared
        switch vault.state {
        case .plain, .unlocked, .new: return true
        case .locked: return unlock()
        case .unavailable:
            showError("Saved logins are unavailable", vault.openError ?? "")
            return false
        }
    }

    /// Makes the vault ready to save: asks how to keep logins the first time, or unlocks it.
    static func ensureWritable() -> Bool {
        let vault = LoginVault.shared
        if vault.state == .new { return chooseStorage() }
        return ensureReadable()
    }

    static func unlock() -> Bool {
        var note = "Enter the Administrator Password to use your saved SSH and camera logins."
        while LoginVault.shared.state == .locked {
            let field = NSSecureTextField(frame: NSRect(x: 0, y: 0, width: 260, height: 24))
            field.setAccessibilityLabel("Administrator Password")
            let alert = NSAlert()
            alert.messageText = "Unlock Saved Logins"
            alert.informativeText = note
            alert.addButton(withTitle: "Unlock")
            alert.addButton(withTitle: "Cancel")
            alert.accessoryView = field
            alert.window.initialFirstResponder = field
            NSApp.activate()
            let answer = alert.runModal() == .alertFirstButtonReturn ? field.stringValue : nil
            field.stringValue = ""
            guard let answer else { return false }
            do { try LoginVault.shared.unlock(answer) } catch { note = "\(error.localizedDescription). Try again." }
        }
        return true
    }

    /// First save: explain, then create an Administrator Password or, after a second
    /// confirmation, keep logins as plain text. Nothing is saved until one is chosen.
    static func chooseStorage() -> Bool {
        let alert = NSAlert()
        alert.messageText = "Protect Saved Logins"
        alert.informativeText = """
            Saved logins hold passwords. An Administrator Password encrypts your saved SSH and camera logins; \
            you enter it once each time PTZ Control starts. It can't be recovered if you forget it.

            Without one, passwords are stored as plain text, which anyone who can read your files can see.
            """
        alert.addButton(withTitle: "Create Administrator Password…")
        alert.addButton(withTitle: "Not Now")
        alert.addButton(withTitle: "Store as Plain Text…")
        NSApp.activate()
        switch alert.runModal() {
        case .alertFirstButtonReturn: return changeAdminPassword()
        case .alertThirdButtonReturn:
            guard confirmPlainText() else { return false }
            do { try LoginVault.shared.setAdminPassword("") } catch {
                showError("Can't save logins", error.localizedDescription)
                return false
            }
            return true
        default: return false
        }
    }

    /// Create Administrator Password, or Change once one is set. Leaving both fields blank
    /// removes it after a confirmation, and logins are then kept as plain text.
    @discardableResult
    static func changeAdminPassword() -> Bool {
        let vault = LoginVault.shared
        guard vault.state != .locked || unlock() else { return false }
        let changing = vault.state == .unlocked
        let first = NSSecureTextField(frame: NSRect(x: 0, y: 30, width: 260, height: 24))
        let second = NSSecureTextField(frame: NSRect(x: 0, y: 0, width: 260, height: 24))
        first.placeholderString = "New Administrator Password"
        second.placeholderString = "Repeat it"
        first.setAccessibilityLabel("New Administrator Password")
        second.setAccessibilityLabel("Repeat the new Administrator Password")
        first.nextKeyView = second
        let fields = NSView(frame: NSRect(x: 0, y: 0, width: 260, height: 54))
        fields.addSubview(first)
        fields.addSubview(second)
        var note = changing
            ? "Every saved login is encrypted again with the new password. Leave both fields blank to remove the Administrator Password."
            : "It encrypts your saved SSH and camera logins. It can't be recovered if you forget it."
        while true {
            let alert = NSAlert()
            alert.messageText = changing ? "Change Administrator Password" : "Create Administrator Password"
            alert.informativeText = note
            alert.addButton(withTitle: changing ? "Change" : "Create")
            alert.addButton(withTitle: "Cancel")
            alert.accessoryView = fields
            alert.window.initialFirstResponder = first
            NSApp.activate()
            let ok = alert.runModal() == .alertFirstButtonReturn
            let a = first.stringValue, b = second.stringValue
            first.stringValue = ""
            second.stringValue = ""
            guard ok else { return false }
            if a != b {
                note = "The two passwords don't match. Try again."
                continue
            }
            if a.isEmpty {
                guard changing else {
                    note = "Enter a password, or Cancel."
                    continue
                }
                guard confirmPlainText() else { return false }
            }
            do {
                try vault.setAdminPassword(a)
                return true
            } catch {
                showError("Can't change the Administrator Password", error.localizedDescription)
                return false
            }
        }
    }

    private static func confirmPlainText() -> Bool {
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = "Store passwords as plain text?"
        alert.informativeText = "Saved SSH and camera passwords will be readable by anyone who can read your files."
        alert.addButton(withTitle: "Cancel")
        alert.addButton(withTitle: "Store as Plain Text")
        return alert.runModal() == .alertSecondButtonReturn
    }

    /// Save login: asks what to call the login, suggesting `suggested`. Saving under an
    /// existing name updates that login.
    static func askName(for store: LoginVault.Store, user: String, suggested: String) -> String? {
        let kind = store == .ssh ? "SSH" : "camera"
        let field = NSTextField(frame: NSRect(x: 0, y: 0, width: 260, height: 24))
        field.stringValue = suggested
        field.placeholderString = store == .ssh ? "e.g. jump host" : "e.g. cam1"
        field.setAccessibilityLabel("Login name")
        var note = "Name the \(kind) login for \(user), to find it in the Login menu. A saved login with the same name is updated."
        while true {
            let alert = NSAlert()
            alert.messageText = "Save \(store == .ssh ? "SSH" : "Camera") Login"
            alert.informativeText = note
            alert.addButton(withTitle: "Save")
            alert.addButton(withTitle: "Don't Save")
            alert.accessoryView = field
            alert.window.initialFirstResponder = field
            NSApp.activate()
            guard alert.runModal() == .alertFirstButtonReturn else { return nil }
            let name = field.stringValue.trimmingCharacters(in: .whitespaces)
            if !name.isEmpty { return name }
            note = "Enter a name, or Don't Save."
        }
    }

    static func showError(_ title: String, _ message: String) {
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = title
        alert.informativeText = message
        NSApp.activate()
        alert.runModal()
    }
}
