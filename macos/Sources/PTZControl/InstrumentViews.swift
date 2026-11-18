import AppKit
import PTZCore

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

/// The camera's live video, or a still picture of its view, with the pan/tilt plane drawn over it at the picture's own
/// scale: once the view is measured at the current zoom, every point of the picture is the pan and tilt it shows, so
/// the grid, the zero axes, the range's limits, the trail, the starting position and the target sit on what they
/// point at, and the current position is the picture's centre. Click or drag to aim at the pan and tilt under the
/// pointer; double-click to aim and move, which brings that point to the centre. Until the view is measured, a
/// double-click measures it first. Nothing covers the picture but thin lines and small labels.
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
    /// Pan and tilt are angles in degrees: the plane is projected as the lens sees it.
    var degrees = false { didSet { needsDisplay = true } }
    /// How wide the picture is at the current zoom; nil until measured.
    var scale: ViewScale? { didSet { needsDisplay = true } }
    /// The camera is turning a little and back to measure its view.
    var measuring = false { didSet { needsDisplay = true } }
    var current: PositionSnapshot? { didSet { recordTrail(); needsDisplay = true } }
    var starting: PositionSnapshot? { didSet { needsDisplay = true } }
    var target: (pan: Double?, tilt: Double?) = (nil, nil) { didSet { needsDisplay = true } }
    var enabled = false { didSet { if !enabled { hover = nil }; needsDisplay = true } }

    var onAim: ((Double, Double) -> Void)?
    var onAimAndMove: ((Double, Double) -> Void)?
    /// A double-click before the view is measured: the point of the picture, −1…1 with up positive.
    var onMeasureAndMove: ((CGPoint) -> Void)?

    private var trail: [CGPoint?] = []  // camera units; nil marks a gap in measurements
    private var hover: CGPoint?
    private var theme = PhosphorTheme.system
    private var tracking: NSTrackingArea?
    private var linesCache: (key: [Double], lines: [PlaneLine])?

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

    /// The picture's frame: as large as fits, in the picture's shape (16:9 until there is one), centred.
    private var plot: CGRect {
        let area = bounds.insetBy(dx: 1, dy: 1)
        let size = picture?.size ?? NSSize(width: 16, height: 9)
        guard size.width > 0, size.height > 0, area.width > 0, area.height > 0 else { return area }
        let scale = min(area.width / size.width, area.height / size.height)
        let w = size.width * scale, h = size.height * scale
        return CGRect(x: area.midX - w / 2, y: area.midY - h / 2, width: w, height: h).integral
    }

    /// Where the camera points and how wide the picture is, once both are known.
    var view: ptz_view? {
        guard let scale, let pan = current?.pan?.value, let tilt = current?.tilt?.value else { return nil }
        func range(_ r: AxisRange?) -> ptz_range { r.map { ptz_range(available: 1, min: $0.min, max: $0.max) } ?? ptz_range() }
        return ptz_view(scale: ptz_view_scale(half_x: scale.halfX, half_y: scale.halfY), degrees: degrees ? 1 : 0,
                        pan: pan, tilt: tilt, pan_range: range(panRange), tilt_range: range(tiltRange))
    }

    /// A point of the view (points) as a point of the picture, −1…1 with up positive, and back.
    private func picturePoint(_ p: CGPoint, in r: CGRect) -> CGPoint {
        CGPoint(x: (p.x - r.midX) / (r.width / 2), y: (p.y - r.midY) / (r.height / 2))
    }
    private func viewPoint(_ p: CGPoint, in r: CGRect) -> CGPoint {
        CGPoint(x: r.midX + p.x * r.width / 2, y: r.midY + p.y * r.height / 2)
    }

    private func cameraPoint(_ p: CGPoint, in r: CGRect) -> (Double, Double)? {
        guard var v = view else { return nil }
        let q = picturePoint(p, in: r)
        var pan = 0.0, tilt = 0.0
        ptz_view_point(&v, Double(q.x), Double(q.y), &pan, &tilt)
        return (pan, tilt)
    }

    private func locate(pan: Double, tilt: Double, in r: CGRect) -> CGPoint? {
        guard var v = view else { return nil }
        var x = 0.0, y = 0.0
        guard ptz_view_locate(&v, pan, tilt, &x, &y) != 0 else { return nil }
        return viewPoint(CGPoint(x: x, y: y), in: r)
    }

    /// The plane's lines for the current view, worked out by the core and kept until the view changes.
    private func lines() -> [PlaneLine] {
        guard var v = view else { return [] }
        let key = [v.scale.half_x, v.scale.half_y, Double(v.degrees), v.pan, v.tilt, v.pan_range.min, v.pan_range.max,
                   Double(v.pan_range.available), v.tilt_range.min, v.tilt_range.max, Double(v.tilt_range.available)]
        if let linesCache, linesCache.key == key { return linesCache.lines }
        var raw = [ptz_plane_line](repeating: ptz_plane_line(), count: 160)
        let count = Int(ptz_view_lines(&v, &raw, Int32(raw.count)))
        let lines = raw.prefix(count).map(PlaneLine.init)
        linesCache = (key, lines)
        return lines
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
        guard enabled, !measuring, panRange != nil, tiltRange != nil else { return }
        let r = plot
        var p = convert(event.locationInWindow, from: nil)
        p.x = min(max(p.x, r.minX), r.maxX)
        p.y = min(max(p.y, r.minY), r.maxY)
        if let (pan, tilt) = cameraPoint(p, in: r) {
            commit ? onAimAndMove?(pan, tilt) : onAim?(pan, tilt)
        } else if commit {
            onMeasureAndMove?(picturePoint(p, in: r))
        }
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

        guard panRange != nil, tiltRange != nil else {
            if enabled { plate("PAN/TILT RANGE UNAVAILABLE", at: CGPoint(x: r.maxX - 6, y: r.minY + 6), color: theme.warning,
                               alignRight: true) }
            drawStatus(plate, in: r)
            return
        }

        // Everything on the plane stays inside the picture, on a dark halo that keeps it readable on any picture.
        NSGraphicsContext.current?.saveGraphicsState()
        NSBezierPath(rect: r).addClip()
        let halo = NSShadow()
        halo.shadowColor = NSColor.black.withAlphaComponent(0.85)
        halo.shadowBlurRadius = 2.5
        halo.shadowOffset = .zero
        halo.set()

        // The grid, the zero axes and the range's limits, where they fall in the picture.
        let lines = enabled ? lines() : []
        if showGrid {
            for line in lines where line.points.count > 1 {
                let path = NSBezierPath()
                path.move(to: viewPoint(line.points[0], in: r))
                for point in line.points.dropFirst() { path.line(to: viewPoint(point, in: r)) }
                switch line.kind {
                case .grid:
                    NSColor.white.withAlphaComponent(0.45).setStroke()
                    path.lineWidth = 0.75
                case .zero:
                    NSColor.white.withAlphaComponent(0.7).setStroke()
                    path.lineWidth = 1
                case .limit:
                    theme.warning.withAlphaComponent(0.9).setStroke()
                    path.lineWidth = 1.5
                    path.setLineDash([6, 3], count: 2, phase: 0)
                }
                path.stroke()
            }
        }

        // Recent measured path; gaps stay gaps.
        let path = NSBezierPath()
        var penDown = false
        for sample in trail {
            guard let sample, let p = locate(pan: sample.x, tilt: sample.y, in: r) else { penDown = false; continue }
            penDown ? path.line(to: p) : path.move(to: p)
            penDown = true
        }
        theme.primary.withAlphaComponent(0.6).setStroke()
        path.lineWidth = 1.5
        path.stroke()

        // Starting position: hollow square marked S.
        if let pan = starting?.pan?.value, let tilt = starting?.tilt?.value, let p = locate(pan: pan, tilt: tilt, in: r) {
            let square = NSBezierPath(rect: CGRect(x: p.x - 5, y: p.y - 5, width: 10, height: 10))
            theme.start.setStroke()
            square.lineWidth = 1.5
            square.stroke()
            NSAttributedString(string: "S", attributes: [.font: mono(9, .bold), .foregroundColor: theme.start])
                .draw(at: CGPoint(x: p.x + 7, y: p.y + 3))
        }

        // Target: dashed crosshair and ring (an axis left blank keeps the current value), with a line from the centre.
        let centre = CGPoint(x: r.midX, y: r.midY)
        if target.pan != nil || target.tilt != nil, let pan = target.pan ?? current?.pan?.value,
           let tilt = target.tilt ?? current?.tilt?.value, let p = locate(pan: pan, tilt: tilt, in: r) {
            theme.target.setStroke()
            let cross = NSBezierPath()
            cross.move(to: CGPoint(x: p.x - 14, y: p.y)); cross.line(to: CGPoint(x: p.x + 14, y: p.y))
            cross.move(to: CGPoint(x: p.x, y: p.y - 14)); cross.line(to: CGPoint(x: p.x, y: p.y + 14))
            cross.appendOval(in: CGRect(x: p.x - 8, y: p.y - 8, width: 16, height: 16))
            cross.lineWidth = 1.5
            cross.setLineDash([4, 2], count: 2, phase: 0)
            cross.stroke()
            let line = NSBezierPath()
            line.move(to: centre); line.line(to: p)
            line.lineWidth = 1
            line.setLineDash([2, 3], count: 2, phase: 0)
            theme.target.withAlphaComponent(0.7).setStroke()
            line.stroke()
        }

        // Where the camera points: the picture's centre, a solid reticle once the view is measured.
        if enabled, current?.pan != nil, current?.tilt != nil {
            theme.primary.setStroke()
            let ring = NSBezierPath(ovalIn: CGRect(x: centre.x - 10, y: centre.y - 10, width: 20, height: 20))
            ring.lineWidth = 1.5
            if scale == nil { ring.setLineDash([3, 3], count: 2, phase: 0) }
            ring.stroke()
            theme.primary.setFill()
            NSBezierPath(ovalIn: CGRect(x: centre.x - 2.5, y: centre.y - 2.5, width: 5, height: 5)).fill()
        }
        NSGraphicsContext.current?.restoreGraphicsState()

        // The lines' values: pan along the top edge, tilt down the right one, each where its line crosses the edge.
        if showGrid {
            var lastPan = -CGFloat.infinity, lastTilt = CGFloat.infinity
            var panPlates: [CGRect] = []
            let font = mono(9, .medium)
            for line in lines.sorted(by: { $0.labelAt.x < $1.labelAt.x }) where line.labelled && line.isPan {
                let text = line.kind == .limit ? "PAN \(line.label) LIMIT" : "PAN \(line.label)"
                let w = NSAttributedString(string: text, attributes: [.font: font]).size().width + 8
                let at = viewPoint(line.labelAt, in: r)
                let x = min(max(at.x - w / 2, r.minX + 4), r.maxX - 4 - w)
                guard x > lastPan + 4 else { continue }
                let y = max(min(at.y, r.maxY) - 19, r.minY + 26)
                plate(text, at: CGPoint(x: x, y: y), color: line.kind == .limit ? theme.warning : .white, size: 9)
                panPlates.append(CGRect(x: x, y: y, width: w, height: 16))
                lastPan = x + w
            }
            for line in lines.sorted(by: { $0.labelAt.y > $1.labelAt.y }) where line.labelled && !line.isPan {
                let text = line.kind == .limit ? "TILT \(line.label) LIMIT" : "TILT \(line.label)"
                let at = viewPoint(line.labelAt, in: r)
                var y = min(max(at.y - 8, r.minY + 26), r.maxY - 40)
                let w = NSAttributedString(string: text, attributes: [.font: font]).size().width + 8
                // Below any pan label it would cover.
                while let hit = panPlates.first(where: { $0.intersects(CGRect(x: min(at.x, r.maxX) - 4 - w, y: y, width: w, height: 16)) }) {
                    y = hit.minY - 18
                }
                guard y < lastTilt - 18, y >= r.minY + 26 else { continue }
                plate(text, at: CGPoint(x: min(at.x, r.maxX) - 4, y: y), color: line.kind == .limit ? theme.warning : .white,
                      alignRight: true, size: 9)
                lastTilt = y
            }
        }
        if enabled, current != nil, current?.pan == nil || current?.tilt == nil {
            plate("POSITION UNAVAILABLE", at: CGPoint(x: r.maxX - 6, y: r.minY + 6), color: theme.warning, alignRight: true)
        } else if enabled, measuring {
            plate("MEASURING THE VIEW · THE CAMERA TURNS A LITTLE AND BACK", at: CGPoint(x: r.minX + 6, y: r.maxY - 42),
                  color: theme.warning)
        } else if enabled, scale == nil, current?.pan != nil {
            plate("VIEW NOT MEASURED AT THIS ZOOM · DOUBLE-CLICK TO MEASURE AND MOVE (⇧⌘M)",
                  at: CGPoint(x: r.minX + 6, y: r.maxY - 42), color: theme.warning)
        }

        drawStatus(plate, in: r)

        // Hover: guides and the pan and tilt under the pointer.
        if let hover, let (pan, tilt) = cameraPoint(hover, in: r) {
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

/// One of the plane's lines from the core, in picture points (−1…1, up positive).
struct PlaneLine {
    enum Kind { case grid, zero, limit }
    let kind: Kind
    let isPan: Bool
    let label: String
    let labelled: Bool
    let labelAt: CGPoint
    let points: [CGPoint]

    init(_ line: ptz_plane_line) {
        let k = Int(line.kind)
        kind = k == PTZ_LINE_PAN_LIMIT || k == PTZ_LINE_TILT_LIMIT ? .limit
            : k == PTZ_LINE_PAN_ZERO || k == PTZ_LINE_TILT_ZERO ? .zero : .grid
        isPan = k == PTZ_LINE_PAN || k == PTZ_LINE_PAN_ZERO || k == PTZ_LINE_PAN_LIMIT
        label = withUnsafeBytes(of: line.label) { String(cString: $0.bindMemory(to: CChar.self).baseAddress!) }
        labelled = line.labelled != 0
        labelAt = CGPoint(x: line.label_x, y: line.label_y)
        let xs = withUnsafeBytes(of: line.x) { Array($0.bindMemory(to: Double.self)) }
        let ys = withUnsafeBytes(of: line.y) { Array($0.bindMemory(to: Double.self)) }
        points = (0..<Int(line.count)).map { CGPoint(x: xs[$0], y: ys[$0]) }
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
