import AppKit

/// Every menu lives in the window, not in the macOS menu bar (§13 H1): wide screens make the trip to the top of the
/// screen long. The menu bar keeps only the app menu macOS always shows; the full menu opens from a ☰ button at the
/// right end of each window's header, and its shortcuts work from any window.
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

    /// The ☰ button, for the right end of a window's header row (as on Linux and Windows).
    static func makeButton() -> NSButton {
        let button = NSButton(frame: NSRect(x: 0, y: 0, width: 34, height: 28))
        button.image = NSImage(systemSymbolName: "line.3.horizontal", accessibilityDescription: "Main Menu")
        button.bezelStyle = .push  // framed, like the window's other buttons and the ☰ on Linux and Windows
        button.imagePosition = .imageOnly
        button.toolTip = "Main Menu: every command of PTZ Control, with its shortcut"
        button.setAccessibilityLabel("Main Menu")
        button.setAccessibilityHelp("Opens every menu of PTZ Control")
        button.target = MenuOpener.shared
        button.action = #selector(MenuOpener.open(_:))
        button.translatesAutoresizingMaskIntoConstraints = false
        button.widthAnchor.constraint(equalToConstant: 38).isActive = true
        return button
    }

    /// The ☰ button of `window`, if it has one (for the tests).
    static func button(in window: NSWindow) -> NSButton? {
        func find(_ view: NSView) -> NSButton? {
            if let b = view as? NSButton, b.action == #selector(MenuOpener.open(_:)) { return b }
            return view.subviews.lazy.compactMap(find).first
        }
        return window.contentView.flatMap(find)
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
        // Right-aligned under the button, which sits at the window's right edge.
        let menu = InWindowMenu.menu
        menu.popUp(positioning: nil, at: NSPoint(x: sender.bounds.width - menu.size.width, y: sender.bounds.height + 4),
                   in: sender)
    }
}
