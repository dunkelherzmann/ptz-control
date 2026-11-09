// Draws the app icon (phosphor reticle on a dark console tile) into an .iconset.
// Usage: swift macos/scripts/make-icon.swift <output.iconset>
import AppKit

let output = URL(fileURLWithPath: CommandLine.arguments[1])
try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)

func render(_ size: Int) -> Data {
    let s = CGFloat(size)
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size, bitsPerSample: 8,
                               samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                               bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let phosphor = NSColor(srgbRed: 0.24, green: 1.0, blue: 0.53, alpha: 1)

    let tile = NSRect(x: s * 0.1, y: s * 0.1, width: s * 0.8, height: s * 0.8)
    let tilePath = NSBezierPath(roundedRect: tile, xRadius: s * 0.18, yRadius: s * 0.18)
    NSGradient(starting: NSColor(srgbRed: 0.09, green: 0.12, blue: 0.11, alpha: 1),
               ending: NSColor(srgbRed: 0.02, green: 0.03, blue: 0.03, alpha: 1))!.draw(in: tilePath, angle: -90)
    phosphor.withAlphaComponent(0.35).setStroke()
    tilePath.lineWidth = max(1, s * 0.008)
    tilePath.stroke()

    // Grid
    let plot = tile.insetBy(dx: s * 0.12, dy: s * 0.12)
    phosphor.withAlphaComponent(0.13).setStroke()
    let grid = NSBezierPath()
    for i in 0...6 {
        let x = plot.minX + plot.width * CGFloat(i) / 6, y = plot.minY + plot.height * CGFloat(i) / 6
        grid.move(to: NSPoint(x: x, y: plot.minY)); grid.line(to: NSPoint(x: x, y: plot.maxY))
        grid.move(to: NSPoint(x: plot.minX, y: y)); grid.line(to: NSPoint(x: plot.maxX, y: y))
    }
    grid.lineWidth = max(0.5, s * 0.004)
    grid.stroke()

    // Axes
    phosphor.withAlphaComponent(0.45).setStroke()
    let axes = NSBezierPath()
    axes.move(to: NSPoint(x: plot.midX, y: plot.minY)); axes.line(to: NSPoint(x: plot.midX, y: plot.maxY))
    axes.move(to: NSPoint(x: plot.minX, y: plot.midY)); axes.line(to: NSPoint(x: plot.maxX, y: plot.midY))
    axes.lineWidth = max(1, s * 0.006)
    axes.stroke()

    // Trail and reticle
    let c = NSPoint(x: plot.minX + plot.width * 0.66, y: plot.minY + plot.height * 0.64)
    let trail = NSBezierPath()
    trail.move(to: NSPoint(x: plot.minX + plot.width * 0.3, y: plot.minY + plot.height * 0.35))
    trail.curve(to: c, controlPoint1: NSPoint(x: plot.minX + plot.width * 0.45, y: plot.minY + plot.height * 0.3),
                controlPoint2: NSPoint(x: plot.minX + plot.width * 0.5, y: plot.minY + plot.height * 0.65))
    phosphor.withAlphaComponent(0.4).setStroke()
    trail.lineWidth = max(1, s * 0.012)
    trail.stroke()

    let shadow = NSShadow()
    shadow.shadowColor = phosphor.withAlphaComponent(0.8)
    shadow.shadowBlurRadius = s * 0.04
    shadow.set()
    phosphor.setStroke()
    let ring = NSBezierPath(ovalIn: NSRect(x: c.x - s * 0.09, y: c.y - s * 0.09, width: s * 0.18, height: s * 0.18))
    ring.lineWidth = max(1, s * 0.016)
    ring.stroke()
    let cross = NSBezierPath()
    cross.move(to: NSPoint(x: c.x - s * 0.15, y: c.y)); cross.line(to: NSPoint(x: c.x - s * 0.1, y: c.y))
    cross.move(to: NSPoint(x: c.x + s * 0.1, y: c.y)); cross.line(to: NSPoint(x: c.x + s * 0.15, y: c.y))
    cross.move(to: NSPoint(x: c.x, y: c.y - s * 0.15)); cross.line(to: NSPoint(x: c.x, y: c.y - s * 0.1))
    cross.move(to: NSPoint(x: c.x, y: c.y + s * 0.1)); cross.line(to: NSPoint(x: c.x, y: c.y + s * 0.15))
    cross.lineWidth = max(1, s * 0.016)
    cross.stroke()
    phosphor.setFill()
    NSBezierPath(ovalIn: NSRect(x: c.x - s * 0.03, y: c.y - s * 0.03, width: s * 0.06, height: s * 0.06)).fill()

    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])!
}

for base in [16, 32, 128, 256, 512] {
    try render(base).write(to: output.appendingPathComponent("icon_\(base)x\(base).png"))
    try render(base * 2).write(to: output.appendingPathComponent("icon_\(base)x\(base)@2x.png"))
}
