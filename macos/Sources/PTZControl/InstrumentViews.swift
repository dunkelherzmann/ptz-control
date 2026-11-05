import AppKit

/// Outlined instrument panel with a caption, e.g. "POSITION PLANE // XY".
final class PanelView: NSView, Themed {
    let contentView = NSView()
    private let captionField = NSTextField(labelWithString: "")
    private var theme = PhosphorTheme.system

    init(caption: String, content: NSView? = nil) {
        super.init(frame: .zero)
        wantsLayer = true
        captionField.stringValue = caption.uppercased()
        captionField.font = Fonts.caption()
        captionField.translatesAutoresizingMaskIntoConstraints = false
        captionField.setContentCompressionResistancePriority(.required, for: .vertical)
        let body = content ?? contentView
        body.translatesAutoresizingMaskIntoConstraints = false
        addSubview(captionField)
        addSubview(body)
        NSLayoutConstraint.activate([
            captionField.topAnchor.constraint(equalTo: topAnchor, constant: 8),
            captionField.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),
            captionField.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor, constant: -12),
            body.topAnchor.constraint(equalTo: captionField.bottomAnchor, constant: 6),
            body.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),
            body.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -12),
            body.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -10),
        ])
    }

    required init?(coder: NSCoder) { fatalError() }

    var caption: String {
        get { captionField.stringValue }
        set { captionField.stringValue = newValue.uppercased() }
    }

    func apply(theme: PhosphorTheme) {
        self.theme = theme
        captionField.textColor = theme.secondary
        needsDisplay = true
    }

    override func updateLayer() {
        layer?.backgroundColor = theme.panel.cgColor
        layer?.borderColor = theme.border.cgColor
        layer?.borderWidth = 1
        layer?.cornerRadius = 6
    }

    override var wantsUpdateLayer: Bool { true }
}

/// Pan/tilt plane in camera units. Click or drag to aim; double-click to aim
/// and move. Only measured positions are drawn: no reading, no marker.
final class PositionPlaneView: NSView, Themed {
    var panRange: AxisRange? { didSet { needsDisplay = true } }
    var tiltRange: AxisRange? { didSet { needsDisplay = true } }
    var current: PositionSnapshot? { didSet { recordTrail(); needsDisplay = true } }
    var starting: PositionSnapshot? { didSet { needsDisplay = true } }
    var target: (pan: Double?, tilt: Double?) = (nil, nil) { didSet { needsDisplay = true } }
    var enabled = false { didSet { needsDisplay = true } }

    var onAim: ((Double, Double) -> Void)?
    var onAimAndMove: ((Double, Double) -> Void)?

    private var trail: [CGPoint?] = []  // camera units; nil marks a gap in measurements
    private var hover: CGPoint?
    private var theme = PhosphorTheme.system
    private var tracking: NSTrackingArea?

    override var isFlipped: Bool { false }
    override var acceptsFirstResponder: Bool { true }

    func apply(theme: PhosphorTheme) {
        self.theme = theme
        needsDisplay = true
    }

    func resetTrail() { trail.removeAll() }

    private func recordTrail() {
        guard let pan = current?.pan?.value, let tilt = current?.tilt?.value else {
            if trail.last != nil { trail.append(nil) }
            return
        }
        let point = CGPoint(x: pan, y: tilt)
        if let last = trail.last, let last, last == point { return }
        trail.append(point)
        if trail.count > 120 { trail.removeFirst(trail.count - 120) }
    }

    private var plot: CGRect {
        bounds.insetBy(dx: 1, dy: 1).inset(left: 44, right: 10, top: 10, bottom: 22)
    }

    private func viewPoint(pan: Double, tilt: Double) -> CGPoint? {
        guard let panRange, let tiltRange, let fx = panRange.fraction(pan), let fy = tiltRange.fraction(tilt) else { return nil }
        let r = plot
        return CGPoint(x: r.minX + r.width * fx, y: r.minY + r.height * fy)
    }

    private func cameraPoint(_ point: CGPoint) -> (Double, Double)? {
        guard let panRange, let tiltRange else { return nil }
        let r = plot
        return (panRange.value(at: (point.x - r.minX) / r.width), tiltRange.value(at: (point.y - r.minY) / r.height))
    }

    override func updateTrackingAreas() {
        if let tracking { removeTrackingArea(tracking) }
        tracking = NSTrackingArea(rect: bounds, options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow],
                                  owner: self, userInfo: nil)
        addTrackingArea(tracking!)
        super.updateTrackingAreas()
    }

    override func mouseMoved(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        hover = plot.contains(p) && enabled ? p : nil
        needsDisplay = true
    }

    override func mouseExited(with event: NSEvent) {
        hover = nil
        needsDisplay = true
    }

    override func mouseDown(with event: NSEvent) { aim(event, commit: event.clickCount >= 2) }
    override func mouseDragged(with event: NSEvent) { aim(event, commit: false) }

    private func aim(_ event: NSEvent, commit: Bool) {
        guard enabled else { return }
        var p = convert(event.locationInWindow, from: nil)
        let r = plot
        p.x = min(max(p.x, r.minX), r.maxX)
        p.y = min(max(p.y, r.minY), r.maxY)
        guard let (pan, tilt) = cameraPoint(p) else { return }
        commit ? onAimAndMove?(pan, tilt) : onAim?(pan, tilt)
    }

    override func draw(_ dirtyRect: NSRect) {
        let r = plot
        let captionAttributes: [NSAttributedString.Key: Any] = [.font: Fonts.mono(9), .foregroundColor: theme.secondary]

        guard let panRange, let tiltRange else {
            let text = enabled ? "PAN/TILT RANGE UNAVAILABLE" : "NO CAMERA LINK"
            let attributed = NSAttributedString(string: text, attributes: [.font: Fonts.mono(12, .semibold),
                                                                          .foregroundColor: theme.secondary])
            let size = attributed.size()
            attributed.draw(at: CGPoint(x: bounds.midX - size.width / 2, y: bounds.midY - size.height / 2))
            drawGrid(in: r)
            return
        }

        drawGrid(in: r)

        // Zero axes, when zero is inside the advertised range.
        theme.axis.setStroke()
        if let zx = viewPoint(pan: 0, tilt: tiltRange.min)?.x {
            let path = NSBezierPath()
            path.move(to: CGPoint(x: zx, y: r.minY))
            path.line(to: CGPoint(x: zx, y: r.maxY))
            path.lineWidth = 1
            path.stroke()
        }
        if let zy = viewPoint(pan: panRange.min, tilt: 0)?.y {
            let path = NSBezierPath()
            path.move(to: CGPoint(x: r.minX, y: zy))
            path.line(to: CGPoint(x: r.maxX, y: zy))
            path.lineWidth = 1
            path.stroke()
        }

        // Range labels.
        func label(_ text: String, _ point: CGPoint, alignRight: Bool = false) {
            let s = NSAttributedString(string: text, attributes: captionAttributes)
            s.draw(at: CGPoint(x: alignRight ? point.x - s.size().width : point.x, y: point.y))
        }
        label(format(panRange.min), CGPoint(x: r.minX, y: r.minY - 16))
        label(format(panRange.max), CGPoint(x: r.maxX, y: r.minY - 16), alignRight: true)
        label("PAN", CGPoint(x: r.midX - 9, y: r.minY - 16))
        label(format(tiltRange.max), CGPoint(x: r.minX - 6, y: r.maxY - 10), alignRight: true)
        label(format(tiltRange.min), CGPoint(x: r.minX - 6, y: r.minY), alignRight: true)
        label("TILT", CGPoint(x: r.minX - 6, y: r.midY - 5), alignRight: true)

        // Recent measured path; gaps stay gaps.
        let path = NSBezierPath()
        var penDown = false
        for sample in trail {
            guard let sample, let p = viewPoint(pan: sample.x, tilt: sample.y) else { penDown = false; continue }
            penDown ? path.line(to: p) : path.move(to: p)
            penDown = true
        }
        theme.primary.withAlphaComponent(0.35).setStroke()
        path.lineWidth = 1.5
        path.stroke()

        // Starting position: hollow square.
        if let pan = starting?.pan?.value, let tilt = starting?.tilt?.value, let p = viewPoint(pan: pan, tilt: tilt) {
            let square = NSBezierPath(rect: CGRect(x: p.x - 5, y: p.y - 5, width: 10, height: 10))
            theme.start.setStroke()
            square.lineWidth = 1.5
            square.stroke()
            label("S", CGPoint(x: p.x + 7, y: p.y + 3))
        }

        // Target: dashed crosshair.
        if let pan = target.pan ?? current?.pan?.value, let tilt = target.tilt ?? current?.tilt?.value,
           target.pan != nil || target.tilt != nil, let p = viewPoint(pan: pan, tilt: tilt) {
            theme.target.setStroke()
            let cross = NSBezierPath()
            cross.move(to: CGPoint(x: p.x - 12, y: p.y))
            cross.line(to: CGPoint(x: p.x + 12, y: p.y))
            cross.move(to: CGPoint(x: p.x, y: p.y - 12))
            cross.line(to: CGPoint(x: p.x, y: p.y + 12))
            cross.lineWidth = 1
            cross.setLineDash([3, 2], count: 2, phase: 0)
            cross.stroke()
            let ring = NSBezierPath(ovalIn: CGRect(x: p.x - 7, y: p.y - 7, width: 14, height: 14))
            ring.setLineDash([3, 2], count: 2, phase: 0)
            ring.stroke()
        }

        // Current measured position: solid reticle.
        if let pan = current?.pan?.value, let tilt = current?.tilt?.value, let p = viewPoint(pan: pan, tilt: tilt) {
            theme.primary.setFill()
            NSBezierPath(ovalIn: CGRect(x: p.x - 4.5, y: p.y - 4.5, width: 9, height: 9)).fill()
            theme.primary.setStroke()
            let ring = NSBezierPath(ovalIn: CGRect(x: p.x - 10, y: p.y - 10, width: 20, height: 20))
            ring.lineWidth = 1.5
            ring.stroke()
        } else if current != nil {
            label("POSITION UNAVAILABLE", CGPoint(x: r.minX + 6, y: r.maxY - 14))
        }

        // Hover readout.
        if let hover, let (pan, tilt) = cameraPoint(hover) {
            theme.axis.setStroke()
            let guide = NSBezierPath()
            guide.move(to: CGPoint(x: hover.x, y: r.minY))
            guide.line(to: CGPoint(x: hover.x, y: r.maxY))
            guide.move(to: CGPoint(x: r.minX, y: hover.y))
            guide.line(to: CGPoint(x: r.maxX, y: hover.y))
            guide.lineWidth = 0.5
            guide.stroke()
            let text = "\(format(pan)), \(format(tilt))"
            let s = NSAttributedString(string: text, attributes: [.font: Fonts.mono(10), .foregroundColor: theme.text])
            let size = s.size()
            var origin = CGPoint(x: hover.x + 8, y: hover.y + 6)
            if origin.x + size.width > r.maxX { origin.x = hover.x - size.width - 8 }
            if origin.y + size.height > r.maxY { origin.y = hover.y - size.height - 6 }
            s.draw(at: origin)
        }
    }

    private func drawGrid(in r: CGRect) {
        theme.grid.setStroke()
        let grid = NSBezierPath()
        for i in 0...10 {
            let x = r.minX + r.width * CGFloat(i) / 10
            let y = r.minY + r.height * CGFloat(i) / 10
            grid.move(to: CGPoint(x: x, y: r.minY))
            grid.line(to: CGPoint(x: x, y: r.maxY))
            grid.move(to: CGPoint(x: r.minX, y: y))
            grid.line(to: CGPoint(x: r.maxX, y: y))
        }
        grid.lineWidth = 0.5
        grid.stroke()
        theme.border.setStroke()
        NSBezierPath(rect: r).stroke()
    }
}

/// Horizontal travel meter: fill = position within the advertised limits.
/// Click or drag along it to aim that axis; double-click to aim and move.
final class AxisMeterView: NSView, Themed {
    var title = "" { didSet { needsDisplay = true } }
    var range: AxisRange? { didSet { needsDisplay = true } }
    var value: Double? { didSet { needsDisplay = true } }
    var start: Double? { didSet { needsDisplay = true } }
    var target: Double? { didSet { needsDisplay = true } }
    var enabled = false { didSet { if !enabled { hover = nil }; needsDisplay = true } }
    /// Snap aimed values to whole numbers (Axis zoom).
    var integer = false

    var onAim: ((Double) -> Void)?
    var onAimAndMove: ((Double) -> Void)?

    private var theme = PhosphorTheme.system
    private var hover: Double?
    private var tracking: NSTrackingArea?

    override var intrinsicContentSize: NSSize { NSSize(width: NSView.noIntrinsicMetric, height: 30) }

    func apply(theme: PhosphorTheme) {
        self.theme = theme
        needsDisplay = true
    }

    private var bar: CGRect { CGRect(x: 44, y: bounds.height - 13, width: bounds.width - 44 - 50, height: 10) }

    /// The axis value under a point, clamped to the advertised limits.
    private func axisValue(at event: NSEvent) -> Double? {
        guard let range else { return nil }
        let x = convert(event.locationInWindow, from: nil).x
        let value = range.value(at: (x - bar.minX) / bar.width)
        return integer ? min(max(value.rounded(), ceil(range.min)), floor(range.max)) : value
    }

    private func isOverBar(_ event: NSEvent) -> Bool {
        let p = convert(event.locationInWindow, from: nil)
        return bar.insetBy(dx: -6, dy: -8).contains(p)
    }

    override func updateTrackingAreas() {
        if let tracking { removeTrackingArea(tracking) }
        tracking = NSTrackingArea(rect: bounds, options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow, .cursorUpdate],
                                  owner: self, userInfo: nil)
        addTrackingArea(tracking!)
        super.updateTrackingAreas()
    }

    override func cursorUpdate(with event: NSEvent) {
        (enabled && range != nil ? NSCursor.resizeLeftRight : NSCursor.arrow).set()
    }

    override func mouseMoved(with event: NSEvent) {
        hover = enabled && isOverBar(event) ? axisValue(at: event) : nil
        needsDisplay = true
    }

    override func mouseExited(with event: NSEvent) {
        hover = nil
        needsDisplay = true
    }

    override func mouseDown(with event: NSEvent) {
        guard enabled, isOverBar(event), let v = axisValue(at: event) else { return }
        event.clickCount >= 2 ? onAimAndMove?(v) : onAim?(v)
    }

    override func mouseDragged(with event: NSEvent) {
        guard enabled, let v = axisValue(at: event) else { return }
        hover = v
        onAim?(v)
    }

    override func draw(_ dirtyRect: NSRect) {
        let label = NSAttributedString(string: title, attributes: [.font: Fonts.mono(10, .semibold),
                                                                   .foregroundColor: theme.secondary])
        label.draw(at: CGPoint(x: 0, y: bounds.height - 13))
        let bar = self.bar
        theme.grid.setFill()
        NSBezierPath(roundedRect: bar, xRadius: 2, yRadius: 2).fill()
        theme.border.setStroke()
        NSBezierPath(roundedRect: bar, xRadius: 2, yRadius: 2).stroke()

        let percent: String
        if let range, let value, let f = range.fraction(value) {
            theme.primary.setFill()
            NSBezierPath(roundedRect: CGRect(x: bar.minX, y: bar.minY, width: max(2, bar.width * f), height: bar.height),
                         xRadius: 2, yRadius: 2).fill()
            percent = String(format: "%3.0f%%", f * 100)
        } else {
            // Unavailable is hatched, never an empty (zero) bar.
            NSGraphicsContext.saveGraphicsState()
            NSBezierPath(rect: bar).addClip()
            theme.secondary.withAlphaComponent(0.4).setStroke()
            let hatch = NSBezierPath()
            var x = bar.minX - bar.height
            while x < bar.maxX {
                hatch.move(to: CGPoint(x: x, y: bar.minY))
                hatch.line(to: CGPoint(x: x + bar.height, y: bar.maxY))
                x += 6
            }
            hatch.stroke()
            NSGraphicsContext.restoreGraphicsState()
            percent = " N/A"
        }
        func tick(_ v: Double?, _ color: NSColor, dashed: Bool) {
            guard let range, let v, let f = range.fraction(v) else { return }
            let x = bar.minX + bar.width * f
            let path = NSBezierPath()
            path.move(to: CGPoint(x: x, y: bar.minY - 3))
            path.line(to: CGPoint(x: x, y: bar.maxY + 3))
            path.lineWidth = 1.5
            if dashed { path.setLineDash([2, 1.5], count: 2, phase: 0) }
            color.setStroke()
            path.stroke()
        }
        tick(start, theme.start, dashed: false)
        tick(target, theme.target, dashed: true)
        // Target handle, so the bar reads as something you can grab.
        if enabled, let range, let target, let f = range.fraction(target) {
            let x = bar.minX + bar.width * f
            theme.target.setFill()
            let handle = NSBezierPath()
            handle.move(to: CGPoint(x: x - 4, y: bar.maxY + 6))
            handle.line(to: CGPoint(x: x + 4, y: bar.maxY + 6))
            handle.line(to: CGPoint(x: x, y: bar.maxY + 1))
            handle.close()
            handle.fill()
        }
        if let hover, let range, let f = range.fraction(hover) {
            let x = bar.minX + bar.width * f
            theme.axis.setStroke()
            let guide = NSBezierPath()
            guide.move(to: CGPoint(x: x, y: bar.minY - 2))
            guide.line(to: CGPoint(x: x, y: bar.maxY + 2))
            guide.lineWidth = 0.5
            guide.stroke()
            let text = NSAttributedString(string: format(hover), attributes: [.font: Fonts.mono(9, .semibold),
                                                                              .foregroundColor: theme.text])
            let w = text.size().width
            text.draw(at: CGPoint(x: min(max(x - w / 2, bar.minX + 30), bar.maxX - w - 30), y: 0))
        }
        NSAttributedString(string: percent, attributes: [.font: Fonts.mono(10), .foregroundColor: theme.text])
            .draw(at: CGPoint(x: bar.maxX + 8, y: bar.minY - 2))
        if let range {
            let attributes: [NSAttributedString.Key: Any] = [.font: Fonts.mono(9), .foregroundColor: theme.secondary]
            NSAttributedString(string: format(range.min), attributes: attributes).draw(at: CGPoint(x: bar.minX, y: 0))
            let maxText = NSAttributedString(string: format(range.max), attributes: attributes)
            maxText.draw(at: CGPoint(x: bar.maxX - maxText.size().width, y: 0))
        }
    }
}

/// Large readout: label, value in the camera's own digits, or an em dash.
final class ReadoutView: NSView, Themed {
    private let label = NSTextField(labelWithString: "")
    private let value = NSTextField(labelWithString: "—")
    private let detail = NSTextField(labelWithString: "")
    private var theme = PhosphorTheme.system
    private var unavailable = true

    init(title: String) {
        super.init(frame: .zero)
        label.stringValue = title
        label.font = Fonts.caption()
        value.font = Fonts.mono(22, .medium)
        value.lineBreakMode = .byTruncatingTail
        detail.font = Fonts.mono(10)
        detail.lineBreakMode = .byTruncatingTail
        let stack = NSStackView(views: [label, value, detail])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 1
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.topAnchor.constraint(equalTo: topAnchor),
            stack.leadingAnchor.constraint(equalTo: leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor),
        ])
    }

    required init?(coder: NSCoder) { fatalError() }

    func set(_ text: String?, detail: String = "") {
        unavailable = text == nil
        value.stringValue = text ?? "—"
        self.detail.stringValue = detail
        apply(theme: theme)
    }

    func apply(theme: PhosphorTheme) {
        self.theme = theme
        label.textColor = theme.secondary
        value.textColor = unavailable ? theme.secondary : theme.primary
        detail.textColor = theme.secondary
    }
}

func format(_ value: Double) -> String {
    if value == value.rounded(), abs(value) < 1e9 { return String(Int64(value)) }
    var text = String(format: "%.4f", value)
    while text.hasSuffix("0") { text.removeLast() }
    if text.hasSuffix(".") { text.removeLast() }
    return text
}

extension CGRect {
    func inset(left: CGFloat, right: CGFloat, top: CGFloat, bottom: CGFloat) -> CGRect {
        CGRect(x: minX + left, y: minY + bottom, width: max(1, width - left - right), height: max(1, height - top - bottom))
    }
}
