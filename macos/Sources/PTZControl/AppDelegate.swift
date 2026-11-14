import AppKit

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuItemValidation {
    private var console: ConsoleWindowController!

    func applicationDidFinishLaunching(_ notification: Notification) {
        console = ConsoleWindowController()
        InWindowMenu.install(full: buildMenu(), appMenu: buildAppMenu(services: true), appName: "PTZ Control")
        if let path = ProcessInfo.processInfo.environment["PTZ_SNAPSHOT_PATH"] {
            // Test copy: render off screen, never on the desktop the user is working on.
            console.runSnapshotScript(to: path)
            return
        }
        console.showWindow(nil)
        NSApp.activate()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        // Close the camera session and SSH tunnel cleanly before quitting.
        guard console.stateValue == .connected || console.stateValue == .connecting else { return .terminateNow }
        console.disconnect { NSApp.reply(toApplicationShouldTerminate: true) }
        return .terminateLater
    }

    func applicationSupportsSecureRestorableState(_ app: NSApplication) -> Bool { true }

    // MARK: Menu

    /// The app menu: in the menu bar (with Services, which macOS keeps there) and first in the ☰ menu.
    private func buildAppMenu(services: Bool) -> NSMenu {
        let appMenu = NSMenu(title: "PTZ Control")
        appMenu.addItem(withTitle: "About PTZ Control", action: #selector(about), keyEquivalent: "").target = self
        appMenu.addItem(.separator())
        if services {
            let servicesMenu = NSMenu()
            appMenu.addItem(withTitle: "Services", action: nil, keyEquivalent: "").submenu = servicesMenu
            NSApp.servicesMenu = servicesMenu
            appMenu.addItem(.separator())
        }
        appMenu.addItem(withTitle: "Hide PTZ Control", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        let others = appMenu.addItem(withTitle: "Hide Others", action: #selector(NSApplication.hideOtherApplications(_:)), keyEquivalent: "h")
        others.keyEquivalentModifierMask = [.command, .option]
        appMenu.addItem(withTitle: "Show All", action: #selector(NSApplication.unhideAllApplications(_:)), keyEquivalent: "")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Quit PTZ Control", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        return appMenu
    }

    /// The ☰ menu in the window: every menu of the app, each as a submenu.
    private func buildMenu() -> NSMenu {
        let main = NSMenu()
        main.addItem(withTitle: "PTZ Control", action: nil, keyEquivalent: "").submenu = buildAppMenu(services: false)

        let edit = NSMenu(title: "Edit")
        edit.addItem(withTitle: "Undo", action: Selector(("undo:")), keyEquivalent: "z")
        edit.addItem(withTitle: "Redo", action: Selector(("redo:")), keyEquivalent: "Z")
        edit.addItem(.separator())
        edit.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        edit.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        edit.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        main.addItem(withTitle: "Edit", action: nil, keyEquivalent: "").submenu = edit

        let camera = NSMenu(title: "Camera")
        func add(_ menu: NSMenu, _ title: String, _ action: Selector, _ key: String,
                 _ modifiers: NSEvent.ModifierFlags = .command) {
            let item = menu.addItem(withTitle: title, action: action, keyEquivalent: key)
            item.keyEquivalentModifierMask = modifiers
            item.target = self
        }
        add(camera, "Connect", #selector(connectToggle), "k")
        add(camera, "Disconnect", #selector(disconnect), "k", [.command, .shift])
        camera.addItem(.separator())
        add(camera, "Move to Target", #selector(move), "\r")
        add(camera, "Return to Starting Coordinates", #selector(returnToStart), "b")
        add(camera, "Stop Motion", #selector(stop), ".")
        camera.addItem(.separator())
        add(camera, "Refresh Position", #selector(refresh), "r")
        add(camera, "Pause Auto-Refresh", #selector(pause), "p")
        add(camera, "Refresh Picture", #selector(refreshPicture), "r", [.command, .shift])
        camera.addItem(.separator())
        add(camera, "Open Camera Web Interface", #selector(web), "l")
        camera.addItem(.separator())
        add(camera, "Manage Logins…", #selector(manageLogins), "l", [.command, .shift])
        main.addItem(withTitle: "Camera", action: nil, keyEquivalent: "").submenu = camera

        let view = NSMenu(title: "View")
        let phosphor = NSMenu(title: "Phosphor")
        for (index, theme) in PhosphorTheme.allCases.enumerated() {
            let item = phosphor.addItem(withTitle: theme.title, action: #selector(chooseTheme(_:)), keyEquivalent: "\(index + 1)")
            item.keyEquivalentModifierMask = [.command, .option]
            item.representedObject = theme.rawValue
            item.target = self
        }
        view.addItem(withTitle: "Phosphor", action: nil, keyEquivalent: "").submenu = phosphor
        main.addItem(withTitle: "View", action: nil, keyEquivalent: "").submenu = view

        let window = NSMenu(title: "Window")
        window.addItem(withTitle: "Minimize", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
        window.addItem(withTitle: "Zoom", action: #selector(NSWindow.performZoom(_:)), keyEquivalent: "")
        window.addItem(withTitle: "Close", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        main.addItem(withTitle: "Window", action: nil, keyEquivalent: "").submenu = window
        NSApp.windowsMenu = window

        let help = NSMenu(title: "Help")
        add(help, "Coordinate Help", #selector(coordinateHelp), "?")
        main.addItem(withTitle: "Help", action: nil, keyEquivalent: "").submenu = help
        return main
    }

    func validateMenuItem(_ item: NSMenuItem) -> Bool {
        let state = console.stateValue
        switch item.action {
        case #selector(connectToggle):
            item.title = state == .connecting ? "Cancel Connection" : "Connect"
            return state == .idle || state == .connecting
        case #selector(disconnect): return state == .connected
        case #selector(move), #selector(returnToStart), #selector(stop), #selector(refresh), #selector(web):
            return state == .connected
        case #selector(pause):
            item.title = console.paused ? "Resume Auto-Refresh" : "Pause Auto-Refresh"
            return state == .connected
        case #selector(refreshPicture): return state == .connected
        case #selector(chooseTheme(_:)):
            item.state = (item.representedObject as? String) == console.theme.rawValue ? .on : .off
            return true
        default: return true
        }
    }

    @objc private func connectToggle() { console.toggleConnection() }
    @objc private func disconnect() { console.disconnect() }
    @objc private func move() { console.moveAction(nil) }
    @objc private func returnToStart() { console.returnToStart(nil) }
    @objc private func stop() { console.stopAction(nil) }
    @objc private func refresh() { console.refreshNow(nil) }
    @objc private func pause() { console.togglePause(nil) }
    @objc private func refreshPicture() { console.refreshPicture(nil) }
    @objc private func web() { console.openWebInterface(nil) }
    @objc private func manageLogins() { ManageLoginsWindowController.shared.present() }

    @objc private func chooseTheme(_ sender: NSMenuItem) {
        if let raw = sender.representedObject as? String, let theme = PhosphorTheme(rawValue: raw) { console.theme = theme }
    }

    static let projectURL = URL(string: "https://github.com/dunkelherzmann/ptz-control")!

    /// PTZ Control ▸ About: the name, the author and a link to the project.
    @objc private func about() {
        let centered = NSMutableParagraphStyle()
        centered.alignment = .center
        let base: [NSAttributedString.Key: Any] = [.font: NSFont.systemFont(ofSize: 11), .foregroundColor: NSColor.labelColor,
                                                   .paragraphStyle: centered]
        let credits = NSMutableAttributedString(string: "PTZ Coordinates Controller\nONVIF · Axis VAPIX · Local or SSH\n\nBy dunkelherzmann\n",
                                                attributes: base)
        var link = base
        link[.link] = Self.projectURL
        credits.append(NSAttributedString(string: Self.projectURL.absoluteString, attributes: link))
        NSApp.orderFrontStandardAboutPanel(options: [.applicationName: "PTZ Control", .credits: credits])
    }

    @objc private func coordinateHelp() {
        let alert = NSAlert()
        alert.messageText = "Coordinate help"
        alert.informativeText = """
        Targets are absolute positions, not increments. Units depend on the vendor:
        • ONVIF: the camera's advertised absolute space (commonly pan/tilt −1…1, zoom 0…1; not degrees).
        • Axis: pan/tilt in degrees; zoom is an integer in the native range.

        Leave a target field blank (or type x) to keep that axis unchanged.
        The camera picture shows the camera's live video, or a still picture when the video can't play: click a point to aim at it, double-click (or Move) to turn the camera so that point comes to the centre. A still picture is renewed once the camera stops. Refresh Picture (⇧⌘R) takes a new picture, or restarts the live view.
        Drag along a travel meter to aim that axis alone, double-click it to aim and move.

        Starting coordinates are the first reading after connecting and stay fixed. Return to Start (⌘B) sends an absolute move there.
        Current coordinates refresh every 2 seconds (read-only) unless paused.
        Stop (⌘.) sends a PTZ stop. Disconnecting never sends a stop.
        Web Interface (⌘L) opens the camera's own page in your browser: HTTP first, HTTPS fallback. No credentials are sent.
        """
        alert.runModal()
    }
}
