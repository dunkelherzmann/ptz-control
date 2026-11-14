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

    /// Controls at the right end of the caption row; the content starts below them.
    func setAccessory(_ accessory: NSView) {
        accessory.translatesAutoresizingMaskIntoConstraints = false
        addSubview(accessory)
        let body = subviews.first { $0 !== captionField && $0 !== accessory }!
        NSLayoutConstraint.activate([
            accessory.topAnchor.constraint(equalTo: topAnchor, constant: 5),
            accessory.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -8),
            captionField.centerYAnchor.constraint(equalTo: accessory.centerYAnchor),
            captionField.trailingAnchor.constraint(lessThanOrEqualTo: accessory.leadingAnchor, constant: -10),
            body.topAnchor.constraint(greaterThanOrEqualTo: accessory.bottomAnchor, constant: 6),
        ])
        constraints.first { $0.firstItem === captionField && $0.firstAttribute == .top }?.isActive = false
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

/// The camera's live video, or a still picture of its view, with the pan/tilt range plane on top: the whole range spans
/// the picture (pan across, tilt up), with its grid, zero axes and limits (hidden with Show Grid off), the trail of
/// recent positions, the starting position, the target and the current position. Click or drag to aim at a pan and tilt;
/// double-click to aim and move. Only measured positions are drawn: no reading, no marker.
final class PositionPlaneView: NSView, Themed {
    var picture: NSImage? { didSet { needsDisplay = true } }
    var pictureTaken: Date? { didSet { needsDisplay = true } }
    /// Set while the picture is the camera's live video, e.g. "H.264 · 1920 × 1080".
    var live: String? { didSet { needsDisplay = true } }
    /// The camera has moved since the picture was taken.
    var pictureStale = false { didSet { needsDisplay = true } }
    /// Shown in place of a picture: why there is none.
    var pictureNote: String? { didSet { needsDisplay = true } }
    var showGrid = true { didSet { needsDisplay = true } }

    var panRange: AxisRange? { didSet { needsDisplay = true } }
    var tiltRange: AxisRange? { didSet { needsDisplay = true } }
    var current: PositionSnapshot? { didSet { recordTrail(); needsDisplay = true } }
    var starting: PositionSnapshot? { didSet { needsDisplay = true } }
    var target: (pan: Double?, tilt: Double?) = (nil, nil) { didSet { needsDisplay = true } }
    var enabled = false { didSet { if !enabled { hover = nil }; needsDisplay = true } }

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

    /// The picture's frame, which the range plane spans: as large as fits, in the picture's shape (16:9 until there is
    /// one), centred.
    private var plot: CGRect {
        let area = bounds.insetBy(dx: 1, dy: 1)
        let size = picture?.size ?? NSSize(width: 16, height: 9)
        guard size.width > 0, size.height > 0, area.width > 0, area.height > 0 else { return area }
        let scale = min(area.width / size.width, area.height / size.height)
        let w = size.width * scale, h = size.height * scale
        return CGRect(x: area.midX - w / 2, y: area.midY - h / 2, width: w, height: h).integral
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
        let mono = { (size: CGFloat, weight: NSFont.Weight) in Fonts.mono(size, weight) }
        func centred(_ text: String, _ y: CGFloat, _ font: NSFont, _ color: NSColor) {
            let s = NSAttributedString(string: text, attributes: [.font: font, .foregroundColor: color])
            s.draw(at: CGPoint(x: r.midX - s.size().width / 2, y: y))
        }
        /// A label on a dark plate, readable on any picture.
        func plate(_ text: String, at origin: CGPoint, color: NSColor = .white, alignRight: Bool = false,
                   size fontSize: CGFloat = 10) {
            let s = NSAttributedString(string: text, attributes: [.font: mono(fontSize, .medium), .foregroundColor: color])
            let size = s.size()
            var o = origin
            if alignRight { o.x -= size.width + 8 }
            NSColor.black.withAlphaComponent(0.55).setFill()
            NSBezierPath(roundedRect: CGRect(x: o.x, y: o.y, width: size.width + 8, height: size.height + 4),
                         xRadius: 3, yRadius: 3).fill()
            s.draw(at: CGPoint(x: o.x + 4, y: o.y + 2))
        }

        // The picture, or why there is none.
        NSColor.black.setFill()
        r.fill()
        if let picture {
            picture.draw(in: r, from: .zero, operation: .sourceOver, fraction: pictureStale ? 0.45 : 1)
        } else {
            let title = !enabled ? "NO CAMERA LINK" : pictureNote == nil ? "TAKING A PICTURE…" : "NO PICTURE"
            centred(title, r.midY + 2, mono(12, .semibold), theme.secondary)
            if enabled, let pictureNote {
                let para = NSMutableParagraphStyle()
                para.alignment = .center
                NSAttributedString(string: pictureNote, attributes: [.font: NSFont.systemFont(ofSize: 11),
                                                                     .foregroundColor: theme.secondary,
                                                                     .paragraphStyle: para])
                    .draw(in: CGRect(x: r.minX + 24, y: r.midY - 44, width: r.width - 48, height: 40))
            }
        }
        NSColor.white.withAlphaComponent(0.25).setStroke()
        NSBezierPath(rect: r.insetBy(dx: 0.5, dy: 0.5)).stroke()

        guard let panRange, let tiltRange else {
            if enabled { plate("PAN/TILT RANGE UNAVAILABLE", at: CGPoint(x: r.maxX - 6, y: r.minY + 6), color: theme.warning,
                               alignRight: true) }
            drawStatus(plate, in: r)
            return
        }

        // The range plane's grid: tenths of the range, the zero axes when zero is in range, and the limits.
        if showGrid {
            let grid = NSBezierPath()
            for i in 1...9 {
                let x = r.minX + r.width * CGFloat(i) / 10, y = r.minY + r.height * CGFloat(i) / 10
                grid.move(to: CGPoint(x: x, y: r.minY)); grid.line(to: CGPoint(x: x, y: r.maxY))
                grid.move(to: CGPoint(x: r.minX, y: y)); grid.line(to: CGPoint(x: r.maxX, y: y))
            }
            NSColor.white.withAlphaComponent(0.18).setStroke()
            grid.lineWidth = 0.5
            grid.stroke()
            let axes = NSBezierPath()
            if let zx = viewPoint(pan: 0, tilt: tiltRange.min)?.x {
                axes.move(to: CGPoint(x: zx, y: r.minY)); axes.line(to: CGPoint(x: zx, y: r.maxY))
            }
            if let zy = viewPoint(pan: panRange.min, tilt: 0)?.y {
                axes.move(to: CGPoint(x: r.minX, y: zy)); axes.line(to: CGPoint(x: r.maxX, y: zy))
            }
            NSColor.white.withAlphaComponent(0.45).setStroke()
            axes.lineWidth = 1
            axes.stroke()
            // Pan limits along the top edge, tilt limits down the right one.
            let top = r.maxY - 19
            plate("PAN \(format(panRange.min))", at: CGPoint(x: r.minX + 4, y: top), size: 9)
            plate("\(format(panRange.max))", at: CGPoint(x: r.maxX - 4, y: top), alignRight: true, size: 9)
            plate("TILT \(format(tiltRange.max))", at: CGPoint(x: r.maxX - 4, y: top - 20), alignRight: true, size: 9)
            plate("\(format(tiltRange.min))", at: CGPoint(x: r.maxX - 4, y: r.minY + 30), alignRight: true, size: 9)
        }

        // A dark halo keeps the markers readable on any picture.
        NSGraphicsContext.current?.saveGraphicsState()
        let halo = NSShadow()
        halo.shadowColor = NSColor.black.withAlphaComponent(0.85)
        halo.shadowBlurRadius = 2.5
        halo.shadowOffset = .zero
        halo.set()

        // Recent measured path; gaps stay gaps.
        let path = NSBezierPath()
        var penDown = false
        for sample in trail {
            guard let sample, let p = viewPoint(pan: sample.x, tilt: sample.y) else { penDown = false; continue }
            penDown ? path.line(to: p) : path.move(to: p)
            penDown = true
        }
        theme.primary.withAlphaComponent(0.6).setStroke()
        path.lineWidth = 1.5
        path.stroke()

        // Starting position: hollow square marked S.
        if let pan = starting?.pan?.value, let tilt = starting?.tilt?.value, let p = viewPoint(pan: pan, tilt: tilt) {
            let square = NSBezierPath(rect: CGRect(x: p.x - 5, y: p.y - 5, width: 10, height: 10))
            theme.start.setStroke()
            square.lineWidth = 1.5
            square.stroke()
            NSAttributedString(string: "S", attributes: [.font: mono(9, .bold), .foregroundColor: theme.start])
                .draw(at: CGPoint(x: p.x + 7, y: p.y + 3))
        }

        // Target: dashed crosshair and ring (an axis left blank keeps the current value).
        let currentPoint = current.flatMap { c -> CGPoint? in
            guard let pan = c.pan?.value, let tilt = c.tilt?.value else { return nil }
            return viewPoint(pan: pan, tilt: tilt)
        }
        if target.pan != nil || target.tilt != nil, let pan = target.pan ?? current?.pan?.value,
           let tilt = target.tilt ?? current?.tilt?.value, let p = viewPoint(pan: pan, tilt: tilt) {
            theme.target.setStroke()
            let cross = NSBezierPath()
            cross.move(to: CGPoint(x: p.x - 14, y: p.y)); cross.line(to: CGPoint(x: p.x + 14, y: p.y))
            cross.move(to: CGPoint(x: p.x, y: p.y - 14)); cross.line(to: CGPoint(x: p.x, y: p.y + 14))
            cross.appendOval(in: CGRect(x: p.x - 8, y: p.y - 8, width: 16, height: 16))
            cross.lineWidth = 1.5
            cross.setLineDash([4, 2], count: 2, phase: 0)
            cross.stroke()
            if let c = currentPoint {
                let line = NSBezierPath()
                line.move(to: c); line.line(to: p)
                line.lineWidth = 1
                line.setLineDash([2, 3], count: 2, phase: 0)
                theme.target.withAlphaComponent(0.7).setStroke()
                line.stroke()
            }
        }

        // Current measured position: solid reticle.
        if let p = currentPoint {
            theme.primary.setStroke()
            let ring = NSBezierPath(ovalIn: CGRect(x: p.x - 10, y: p.y - 10, width: 20, height: 20))
            ring.lineWidth = 1.5
            ring.stroke()
            theme.primary.setFill()
            NSBezierPath(ovalIn: CGRect(x: p.x - 4.5, y: p.y - 4.5, width: 9, height: 9)).fill()
        }
        NSGraphicsContext.current?.restoreGraphicsState()
        if current != nil, currentPoint == nil {
            plate("POSITION UNAVAILABLE", at: CGPoint(x: r.maxX - 6, y: r.minY + 6), color: theme.warning, alignRight: true)
        }

        drawStatus(plate, in: r)

        // Hover: guides and the pan and tilt under the pointer.
        if let hover, let (pan, tilt) = cameraPoint(hover) {
            let guide = NSBezierPath()
            guide.move(to: CGPoint(x: hover.x, y: r.minY)); guide.line(to: CGPoint(x: hover.x, y: r.maxY))
            guide.move(to: CGPoint(x: r.minX, y: hover.y)); guide.line(to: CGPoint(x: r.maxX, y: hover.y))
            NSColor.white.withAlphaComponent(0.35).setStroke()
            guide.lineWidth = 0.5
            guide.stroke()
            let text = "PAN \(format(pan))  TILT \(format(tilt))"
            let width = NSAttributedString(string: text, attributes: [.font: mono(10, .medium)]).size().width + 18
            var origin = CGPoint(x: hover.x + 10, y: hover.y + 8)
            if origin.x + width > r.maxX { origin.x = hover.x - width - 10 }
            if origin.y + 22 > r.maxY { origin.y = hover.y - 26 }
            plate(text, at: origin)
        }
    }

    /// The plate in the lower left: live, when the picture was taken, or that it is out of date.
    private func drawStatus(_ plate: (String, CGPoint, NSColor, Bool, CGFloat) -> Void, in r: CGRect) {
        guard picture != nil else { return }
        let status = live.map { "● LIVE · \($0)" } ?? (pictureStale ? "MOVING · NEW PICTURE WHEN IT STOPS"
            : pictureTaken.map { "PICTURE \(Self.time.string(from: $0))" } ?? "PICTURE")
        plate(status, CGPoint(x: r.minX + 6, y: r.minY + 6), pictureStale ? theme.warning : .white, false, 10)
    }

    private static let time: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "HH:mm:ss"
        return f
    }()
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
