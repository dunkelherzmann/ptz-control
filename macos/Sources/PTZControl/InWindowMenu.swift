import AppKit

/// Every menu lives in the window, not in the macOS menu bar (§13 H1): wide screens make the trip to the top of the
/// screen long. The menu bar keeps only the app menu macOS always shows; the full menu opens from a ☰ button beside
/// the window controls, and its shortcuts work from any window.
@MainActor
enum InWindowMenu {
    /// The full menu: one submenu per former menu-bar menu.
    private(set) static var menu = NSMenu()
    private static var monitor: Any?

    /// Shows only `appMenu` in the menu bar and routes key equivalents to `full`.
    static func install(full: NSMenu, appMenu: NSMenu, appName: String) {
        menu = full
        let bar = NSMenu()
        bar.addItem(withTitle: appName, action: nil, keyEquivalent: "").submenu = appMenu
        NSApp.mainMenu = bar
        if monitor == nil {
            monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { event in
                route(event) ? nil : event
            }
        }
    }

    /// AppKit's own order: the key window (its fields, buttons and sheets) first, then the in-window menu, which
    /// takes the place of the menu bar. Returns whether the event was handled.
    static func route(_ event: NSEvent) -> Bool {
        let modifiers = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        guard modifiers.contains(.command) || modifiers.contains(.control) else { return false }
        if let window = NSApp.keyWindow, window.performKeyEquivalent(with: event) { return true }
        return menu.performKeyEquivalent(with: event)
    }

    /// Puts the ☰ button beside the window controls.
    static func attach(to window: NSWindow) {
        let button = NSButton(frame: NSRect(x: 0, y: 0, width: 30, height: 22))
        button.image = NSImage(systemSymbolName: "line.3.horizontal", accessibilityDescription: "Main Menu")
        button.isBordered = false
        button.bezelStyle = .accessoryBarAction
        button.showsBorderOnlyWhileMouseInside = true
        button.toolTip = "Main Menu: every command of PTZ Control, with its shortcut"
        button.setAccessibilityLabel("Main Menu")
        button.setAccessibilityHelp("Opens every menu of PTZ Control")
        button.target = MenuOpener.shared
        button.action = #selector(MenuOpener.open(_:))
        let holder = NSView(frame: NSRect(x: 0, y: 0, width: 38, height: 28))
        button.frame.origin = NSPoint(x: 4, y: 3)
        holder.addSubview(button)
        let accessory = NSTitlebarAccessoryViewController()
        accessory.view = holder
        accessory.layoutAttribute = .leading
        window.addTitlebarAccessoryViewController(accessory)
    }

    /// The ☰ button of `window`, if it has one (for the tests).
    static func button(in window: NSWindow) -> NSButton? {
        window.titlebarAccessoryViewControllers.lazy.compactMap { $0.view.subviews.first as? NSButton }
            .first { $0.action == #selector(MenuOpener.open(_:)) }
    }

    /// The menu tree as indented lines, for the tests.
    static func outline(_ menu: NSMenu? = nil, depth: Int = 0) -> [String] {
        (menu ?? self.menu).items.flatMap { item -> [String] in
            guard !item.isSeparatorItem else { return [] }
            let line = String(repeating: "  ", count: depth) + item.title
            return [line] + (item.submenu.map { outline($0, depth: depth + 1) } ?? [])
        }
    }
}

@MainActor
private final class MenuOpener: NSObject {
    static let shared = MenuOpener()
    @objc func open(_ sender: NSButton) {
        InWindowMenu.menu.popUp(positioning: nil, at: NSPoint(x: 0, y: sender.bounds.height + 4), in: sender)
    }
}
