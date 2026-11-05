import AppKit

/// Console appearances. Instruments are drawn by the app, so the phosphor is
/// part of the product: system colours, or a monochrome CRT-style phosphor.
enum PhosphorTheme: String, CaseIterable {
    case system, green, amber, blue, mono

    var title: String {
        switch self {
        case .system: return "System"
        case .green: return "Green Phosphor"
        case .amber: return "Amber Phosphor"
        case .blue: return "Blue Phosphor"
        case .mono: return "Mono"
        }
    }

    /// nil follows the system light/dark setting.
    var appearance: NSAppearance? {
        self == .system ? nil : NSAppearance(named: .darkAqua)
    }

    private static func rgb(_ hex: UInt32, _ alpha: CGFloat = 1) -> NSColor {
        NSColor(srgbRed: CGFloat((hex >> 16) & 0xff) / 255, green: CGFloat((hex >> 8) & 0xff) / 255,
                blue: CGFloat(hex & 0xff) / 255, alpha: alpha)
    }

    private var phosphor: NSColor? {
        switch self {
        case .system: return nil
        case .green: return Self.rgb(0x3DFF86)
        case .amber: return Self.rgb(0xFFB33D)
        case .blue: return Self.rgb(0x5CC8FF)
        case .mono: return Self.rgb(0xE6E6E6)
        }
    }

    var background: NSColor { phosphor == nil ? .windowBackgroundColor : Self.rgb(0x07090A) }
    var panel: NSColor { phosphor == nil ? .controlBackgroundColor : Self.rgb(0x0D1112) }
    var border: NSColor { phosphor?.withAlphaComponent(0.28) ?? .separatorColor }
    var grid: NSColor { phosphor?.withAlphaComponent(0.12) ?? NSColor.separatorColor.withAlphaComponent(0.6) }
    var axis: NSColor { phosphor?.withAlphaComponent(0.45) ?? .tertiaryLabelColor }
    /// Live measurements.
    var primary: NSColor { phosphor ?? .controlAccentColor }
    /// Captions, units, secondary text.
    var secondary: NSColor { phosphor?.withAlphaComponent(0.6) ?? .secondaryLabelColor }
    var text: NSColor { phosphor?.blended(withFraction: 0.55, of: .white) ?? .labelColor }
    /// Starting position marker.
    var start: NSColor { phosphor?.withAlphaComponent(0.7) ?? .systemOrange }
    /// Operator's target marker.
    var target: NSColor { phosphor?.blended(withFraction: 0.5, of: .white) ?? .systemPink }
    var warning: NSColor { self == .system ? .systemRed : Self.rgb(0xFF5A4F) }
    var good: NSColor { phosphor ?? .systemGreen }
}

/// Views that redraw with the active theme.
protocol Themed: AnyObject {
    func apply(theme: PhosphorTheme)
}

extension NSView {
    func applyThemeRecursively(_ theme: PhosphorTheme) {
        (self as? Themed)?.apply(theme: theme)
        for subview in subviews { subview.applyThemeRecursively(theme) }
    }
}

enum Fonts {
    static func mono(_ size: CGFloat, _ weight: NSFont.Weight = .regular) -> NSFont {
        .monospacedSystemFont(ofSize: size, weight: weight)
    }

    static func caption() -> NSFont { .monospacedSystemFont(ofSize: 10, weight: .semibold) }
}
