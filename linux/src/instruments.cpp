// Position plane and travel meters. Same drawing as the macOS and Windows hosts.
#include "instruments.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

Theme g_theme = make_theme(Phosphor::System);

QColor alpha(QColor c, int a) {
    c.setAlpha(a);
    return c;
}

void draw_text(QPainter &p, const QString &text, double x, double y, double size, const QColor &color,
               bool bold = false, bool right = false) {
    p.setFont(mono_font(size, bold));
    p.setPen(color);
    QFontMetricsF m(p.font());
    double w = m.horizontalAdvance(text);
    p.drawText(QPointF(right ? x - w : x, y + m.ascent()), text);
}

double text_width(const QString &text, double size, bool bold = false) {
    return QFontMetricsF(mono_font(size, bold)).horizontalAdvance(text);
}

QPen pen(const QColor &c, double width, Qt::PenStyle style = Qt::SolidLine) {
    QPen p(c, width, style);
    p.setCapStyle(Qt::FlatCap);
    return p;
}

}  // namespace

void instruments_set_theme(const Theme &theme) { g_theme = theme; }

// ---- position plane ------------------------------------------------------------

PlaneView::PlaneView(QWidget *parent) : QWidget(parent) {
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    setMinimumHeight(140);
}

// The picture's frame: as large as fits, in the picture's shape (16:9 until there is
// one), centred.
QRectF PlaneView::plot_rect() const {
    QRectF area = QRectF(rect()).adjusted(1, 1, -1, -1);
    double pw = state.picture.isNull() ? 16 : state.picture.width(), ph = state.picture.isNull() ? 9 : state.picture.height();
    if (pw <= 0 || ph <= 0 || area.width() <= 0 || area.height() <= 0) return area;
    double scale = std::min(area.width() / pw, area.height() / ph), w = std::round(pw * scale), h = std::round(ph * scale);
    return QRectF(std::round(area.center().x() - w / 2), std::round(area.center().y() - h / 2), w, h);
}

namespace {

// A point of the widget as a point of the picture, -1…1 with up positive, and back.
QPointF picture_point(QPointF p, const QRectF &r) {
    return QPointF((p.x() - r.center().x()) / (r.width() / 2), (r.center().y() - p.y()) / (r.height() / 2));
}
QPointF widget_point(QPointF p, const QRectF &r) {
    return QPointF(r.center().x() + p.x() * r.width() / 2, r.center().y() - p.y() * r.height() / 2);
}

}  // namespace

std::optional<ptz_view> PlaneView::view() const {
    const auto &c = state.current;
    if (!state.scale || !c || !c->pan || !c->tilt) return std::nullopt;
    auto range = [](const AxisRange &r) { return ptz_range{r.available ? 1 : 0, r.min, r.max}; };
    return ptz_view{{state.scale->half_x, state.scale->half_y}, state.degrees ? 1 : 0, c->pan->value, c->tilt->value,
                    range(state.pan), range(state.tilt)};
}

std::optional<QPointF> PlaneView::camera_point(QPointF at, const QRectF &r) const {
    auto v = view();
    if (!v) return std::nullopt;
    QPointF q = picture_point(at, r);
    double pan = 0, tilt = 0;
    ptz_view_point(&*v, q.x(), q.y(), &pan, &tilt);
    return QPointF(pan, tilt);
}

std::optional<QPointF> PlaneView::locate(double pan, double tilt, const QRectF &r) const {
    auto v = view();
    if (!v) return std::nullopt;
    double x = 0, y = 0;
    if (!ptz_view_locate(&*v, pan, tilt, &x, &y)) return std::nullopt;
    return widget_point(QPointF(x, y), r);
}

// The plane's lines for the current view, worked out by the core and kept until the view changes.
const std::vector<PlaneLine> &PlaneView::lines() {
    auto v = view();
    if (!v) {
        lines_key_.clear();
        lines_.clear();
        return lines_;
    }
    std::vector<double> key{v->scale.half_x, v->scale.half_y, double(v->degrees), v->pan, v->tilt,
                            v->pan_range.min, v->pan_range.max, double(v->pan_range.available),
                            v->tilt_range.min, v->tilt_range.max, double(v->tilt_range.available)};
    if (key == lines_key_) return lines_;
    std::vector<ptz_plane_line> raw(160);
    int count = ptz_view_lines(&*v, raw.data(), static_cast<int>(raw.size()));
    lines_.clear();
    for (int i = 0; i < count; ++i) {
        const ptz_plane_line &l = raw[i];
        PlaneLine line;
        line.kind = l.kind == PTZ_LINE_PAN_LIMIT || l.kind == PTZ_LINE_TILT_LIMIT ? PlaneLine::Limit
                    : l.kind == PTZ_LINE_PAN_ZERO || l.kind == PTZ_LINE_TILT_ZERO ? PlaneLine::Zero
                                                                                  : PlaneLine::Grid;
        line.pan = l.kind == PTZ_LINE_PAN || l.kind == PTZ_LINE_PAN_ZERO || l.kind == PTZ_LINE_PAN_LIMIT;
        line.label = QString::fromUtf8(l.label, static_cast<int>(strnlen(l.label, sizeof l.label)));
        line.labelled = l.labelled != 0;
        line.label_at = QPointF(l.label_x, l.label_y);
        for (int j = 0; j < l.count && j < PTZ_LINE_POINTS; ++j) line.points.emplace_back(l.x[j], l.y[j]);
        lines_.push_back(std::move(line));
    }
    lines_key_ = key;
    return lines_;
}

void PlaneView::record() {
    const auto &current = state.current;
    if (!current || !current->pan || !current->tilt) {
        if (!trail_.empty() && trail_.back()) trail_.push_back(std::nullopt);
        return;
    }
    QPointF point(current->pan->value, current->tilt->value);
    if (!trail_.empty() && trail_.back() && *trail_.back() == point) return;
    trail_.push_back(point);
    if (trail_.size() > 120) trail_.erase(trail_.begin(), trail_.end() - 120);
}

void PlaneView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), g_theme.panel);
    const PlaneState &s = state;
    QRectF r = plot_rect();
    const QColor white(255, 255, 255);
    auto centred = [&](const QString &text, double y, double size, bool bold, const QColor &color) {
        draw_text(p, text, r.center().x() - text_width(text, size, bold) / 2, y, size, color, bold);
    };
    // A label on a dark plate, readable on any picture. `at` is the plate's top-left (or top-right) corner.
    auto plate_height = [](double size = 10) { return QFontMetricsF(mono_font(size, true)).height() + 4; };
    auto plate = [&](const QString &text, QPointF at, const QColor &color, bool align_right = false, double size = 10) {
        QFontMetricsF m(mono_font(size, true));
        double w = m.horizontalAdvance(text) + 8, h = m.height() + 4;
        if (align_right) at.rx() -= w;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 140));
        p.drawRoundedRect(QRectF(at.x(), at.y(), w, h), 3, 3);
        p.setBrush(Qt::NoBrush);
        draw_text(p, text, at.x() + 4, at.y() + 2, size, color, true);
    };
    // The plate in the lower left: live, when the picture was taken, or that it is out of date.
    auto status = [&] {
        if (s.picture.isNull()) return;
        QString text = !s.live.isEmpty() ? "● LIVE · " + s.live
                       : s.picture_stale ? "MOVING · NEW PICTURE WHEN IT STOPS"
                       : s.picture_taken.isValid() ? "PICTURE " + s.picture_taken.toString("HH:mm:ss") : QString("PICTURE");
        plate(text, QPointF(r.x() + 6, r.bottom() - 6 - plate_height()), s.picture_stale ? g_theme.warning : white);
    };

    // The picture, or why there is none.
    p.fillRect(r, Qt::black);
    if (!s.picture.isNull()) {
        p.save();
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.setOpacity(s.picture_stale ? 0.45 : 1);
        p.drawImage(r, s.picture);
        p.restore();
    } else {
        QString title = !s.enabled ? "NO CAMERA LINK" : s.picture_note.isEmpty() ? "TAKING A PICTURE…" : "NO PICTURE";
        double th = QFontMetricsF(mono_font(12, true)).height();
        centred(title, r.center().y() - th - 2, 12, true, g_theme.secondary);
        if (s.enabled && !s.picture_note.isEmpty()) {
            QFont f;
            f.setPixelSize(11);
            p.setFont(f);
            p.setPen(g_theme.secondary);
            p.drawText(QRectF(r.x() + 24, r.center().y() + 4, r.width() - 48, 40), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                       s.picture_note);
        }
    }
    p.setPen(pen(alpha(white, 64), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));

    if (!s.pan.available || !s.tilt.available) {
        if (s.enabled)
            plate("PAN/TILT RANGE UNAVAILABLE", QPointF(r.right() - 6, r.bottom() - 6 - plate_height()), g_theme.warning, true);
        status();
        return;
    }

    // A dark halo under each line and marker keeps it readable on any picture.
    auto stroke = [&](const QPainterPath &path, QPen ink) {
        QPen halo = ink;
        halo.setColor(QColor(0, 0, 0, 110));
        halo.setWidthF(ink.widthF() + 2);
        if (ink.style() == Qt::CustomDashLine) {  // the same dashes, measured in the wider pen's widths
            QList<qreal> dashes = ink.dashPattern();
            for (qreal &d : dashes) d *= ink.widthF() / halo.widthF();
            halo.setDashPattern(dashes);
        }
        p.strokePath(path, halo);
        p.strokePath(path, ink);
    };

    // Everything on the plane stays inside the picture.
    p.save();
    p.setClipRect(r);

    // The grid, the zero axes and the range's limits, where they fall in the picture.
    const std::vector<PlaneLine> no_lines;
    const std::vector<PlaneLine> &plane_lines = s.enabled ? lines() : no_lines;
    if (s.show_grid) {
        for (const PlaneLine &line : plane_lines) {
            if (line.points.size() < 2) continue;
            QPainterPath path;
            path.moveTo(widget_point(line.points[0], r));
            for (size_t i = 1; i < line.points.size(); ++i) path.lineTo(widget_point(line.points[i], r));
            switch (line.kind) {
                case PlaneLine::Grid: stroke(path, pen(alpha(white, 115), 0.75)); break;
                case PlaneLine::Zero: stroke(path, pen(alpha(white, 178), 1)); break;
                case PlaneLine::Limit: {
                    QPen dashed = pen(alpha(g_theme.warning, 230), 1.5);
                    dashed.setDashPattern({6 / 1.5, 3 / 1.5});
                    stroke(path, dashed);
                    break;
                }
            }
        }
    }

    // Recent measured path; gaps stay gaps.
    QPainterPath trail;
    bool pen_down = false;
    for (const auto &sample : trail_) {
        std::optional<QPointF> point = sample ? locate(sample->x(), sample->y(), r) : std::nullopt;
        if (!point) {
            pen_down = false;
            continue;
        }
        pen_down ? trail.lineTo(*point) : trail.moveTo(*point);
        pen_down = true;
    }
    QPen trail_pen = pen(alpha(g_theme.primary, 153), 1.5);
    trail_pen.setJoinStyle(Qt::RoundJoin);
    stroke(trail, trail_pen);

    // Starting position: hollow square marked S.
    if (s.starting && s.starting->pan && s.starting->tilt) {
        if (auto sp = locate(s.starting->pan->value, s.starting->tilt->value, r)) {
            QPainterPath square;
            square.addRect(QRectF(sp->x() - 5, sp->y() - 5, 10, 10));
            stroke(square, pen(g_theme.start, 1.5));
            QPainterPath letter;
            QFont font = mono_font(9, true);
            letter.addText(QPointF(sp->x() + 7, sp->y() - 3), font, "S");
            p.strokePath(letter, QPen(QColor(0, 0, 0, 110), 2));
            p.fillPath(letter, g_theme.start);
        }
    }

    // Target: dashed crosshair and ring (an axis left blank keeps the current value), with a line from the centre.
    const QPointF centre = r.center();
    if (s.target_pan || s.target_tilt) {
        std::optional<double> pan = s.target_pan, tilt = s.target_tilt;
        if (!pan && s.current && s.current->pan) pan = s.current->pan->value;
        if (!tilt && s.current && s.current->tilt) tilt = s.current->tilt->value;
        if (pan && tilt) {
            if (auto t = locate(*pan, *tilt, r)) {
                QPainterPath cross;
                cross.moveTo(t->x() - 14, t->y());
                cross.lineTo(t->x() + 14, t->y());
                cross.moveTo(t->x(), t->y() - 14);
                cross.lineTo(t->x(), t->y() + 14);
                cross.addEllipse(*t, 8, 8);
                QPen dashed = pen(g_theme.target, 1.5);
                dashed.setDashPattern({4 / 1.5, 2 / 1.5});
                stroke(cross, dashed);
                QPainterPath line;
                line.moveTo(centre);
                line.lineTo(*t);
                QPen dotted = pen(alpha(g_theme.target, 178), 1);
                dotted.setDashPattern({2, 3});
                stroke(line, dotted);
            }
        }
    }

    // Where the camera points: the picture's centre, a solid reticle once the view is measured.
    bool positioned = s.current && s.current->pan && s.current->tilt;
    if (s.enabled && positioned) {
        QPainterPath ring;
        ring.addEllipse(centre, 10, 10);
        QPen ring_pen = pen(g_theme.primary, 1.5);
        if (!s.scale) ring_pen.setDashPattern({3 / 1.5, 3 / 1.5});
        stroke(ring, ring_pen);
        QPainterPath dot;
        dot.addEllipse(centre, 2.5, 2.5);
        p.strokePath(dot, QPen(QColor(0, 0, 0, 110), 2));
        p.fillPath(dot, g_theme.primary);
    }
    p.restore();

    // The lines' values: pan along the top edge, tilt down the right one, each where its line crosses the edge.
    const double label_h = plate_height(9);
    if (s.show_grid) {
        std::vector<const PlaneLine *> pans, tilts;
        for (const PlaneLine &line : plane_lines)
            if (line.labelled) (line.pan ? pans : tilts).push_back(&line);
        std::sort(pans.begin(), pans.end(), [](auto a, auto b) { return a->label_at.x() < b->label_at.x(); });
        std::sort(tilts.begin(), tilts.end(), [](auto a, auto b) { return a->label_at.y() > b->label_at.y(); });
        auto width = [](const QString &text) { return text_width(text, 9, true) + 8; };
        double last_pan = -1e9;
        std::vector<QRectF> pan_plates;
        for (const PlaneLine *line : pans) {
            QString text = "PAN " + line->label + (line->kind == PlaneLine::Limit ? " LIMIT" : "");
            double w = width(text);
            QPointF at = widget_point(line->label_at, r);
            double x = std::min(std::max(at.x() - w / 2, r.left() + 4), r.right() - 4 - w);
            if (x <= last_pan + 4) continue;
            double y = std::min(std::max(at.y(), r.top()) + 4, r.bottom() - 26 - label_h);
            plate(text, QPointF(x, y), line->kind == PlaneLine::Limit ? g_theme.warning : white, false, 9);
            pan_plates.emplace_back(x, y, w, label_h);
            last_pan = x + w;
        }
        double last_tilt = -1e9;
        for (const PlaneLine *line : tilts) {
            QString text = "TILT " + line->label + (line->kind == PlaneLine::Limit ? " LIMIT" : "");
            double w = width(text);
            QPointF at = widget_point(line->label_at, r);
            double right = std::min(at.x(), r.right()) - 4;
            double y = std::max(std::min(at.y() - label_h / 2, r.bottom() - 26 - label_h), r.top() + 24);
            // Below any pan label it would cover.
            for (bool moved = true; moved;) {
                moved = false;
                for (const QRectF &hit : pan_plates)
                    if (hit.intersects(QRectF(right - w, y, w, label_h))) {
                        y = hit.bottom() + 2;
                        moved = true;
                    }
            }
            if (y < last_tilt + label_h + 2 || y > r.bottom() - 26 - label_h) continue;
            plate(text, QPointF(right, y), line->kind == PlaneLine::Limit ? g_theme.warning : white, true, 9);
            last_tilt = y;
        }
    }
    if (s.enabled && s.current && !positioned) {
        plate("POSITION UNAVAILABLE", QPointF(r.right() - 6, r.bottom() - 6 - plate_height()), g_theme.warning, true);
    } else if (s.enabled && s.measuring) {
        plate("MEASURING THE VIEW · THE CAMERA TURNS A LITTLE AND BACK", QPointF(r.x() + 6, r.top() + 26), g_theme.warning);
    } else if (s.enabled && !s.scale && positioned) {
        plate("VIEW NOT MEASURED AT THIS ZOOM · DOUBLE-CLICK TO MEASURE AND MOVE (Ctrl+Shift+M)",
              QPointF(r.x() + 6, r.top() + 26), g_theme.warning);
    }

    status();

    // Hover: guides and the pan and tilt under the pointer.
    if (hover_ && s.enabled && r.contains(*hover_)) {
        if (auto camera = camera_point(*hover_, r)) {
            QPointF h = *hover_;
            p.setPen(pen(alpha(white, 89), 0.5));
            p.drawLine(QPointF(h.x(), r.y()), QPointF(h.x(), r.bottom()));
            p.drawLine(QPointF(r.x(), h.y()), QPointF(r.right(), h.y()));
            QString text = "PAN " + format_value(camera->x()) + "  TILT " + format_value(camera->y());
            double width = QFontMetricsF(mono_font(10, true)).horizontalAdvance(text) + 18;
            QPointF at(h.x() + 10, h.y() - 8 - plate_height());
            if (at.x() + width > r.right()) at.setX(h.x() - width - 10);
            if (at.y() < r.y()) at.setY(h.y() + 8);
            plate(text, at, white);
        }
    }
}

void PlaneView::aim(QPointF at, bool commit) {
    if (!state.enabled || state.measuring || !state.pan.available || !state.tilt.available) return;
    QRectF r = plot_rect();
    if (r.isEmpty()) return;
    at = QPointF(std::clamp(at.x(), r.left(), r.right()), std::clamp(at.y(), r.top(), r.bottom()));
    if (auto camera = camera_point(at, r)) {
        if (commit) {
            if (state.on_aim_and_move) state.on_aim_and_move(camera->x(), camera->y());
        } else if (state.on_aim) {
            state.on_aim(camera->x(), camera->y());
        }
    } else if (commit && state.on_measure_and_move) {
        state.on_measure_and_move(picture_point(at, r));
    }
}

void PlaneView::mouseMoveEvent(QMouseEvent *e) {
    QPointF at = e->position();
    hover_ = state.enabled && plot_rect().contains(at) ? std::optional<QPointF>(at) : std::nullopt;
    if (dragging_) aim(at, false);
    update();
}

void PlaneView::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) return;
    dragging_ = true;
    aim(e->position(), false);
}

void PlaneView::mouseReleaseEvent(QMouseEvent *) { dragging_ = false; }

void PlaneView::mouseDoubleClickEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) aim(e->position(), true);
}

void PlaneView::leaveEvent(QEvent *) {
    hover_.reset();
    update();
}

// ---- travel meter ----------------------------------------------------------------

MeterView::MeterView(const QString &title, QWidget *parent) : QWidget(parent), title_(title) {
    setMouseTracking(true);
    setMinimumHeight(34);
}

QRectF MeterView::bar_rect() const { return QRectF(44, 6, std::max(1.0, width() - 44.0 - 50.0), 10); }

std::optional<double> MeterView::value_at(double x) const {
    if (!state.range.available) return std::nullopt;
    QRectF bar = bar_rect();
    double v = state.range.value_at((x - bar.x()) / bar.width());
    if (state.integer) v = std::min(std::max(std::round(v), std::ceil(state.range.min)), std::floor(state.range.max));
    return v;
}

void MeterView::refresh_cursor() {
    setCursor(state.enabled && state.range.available ? Qt::SizeHorCursor : Qt::ArrowCursor);
}

void MeterView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), g_theme.panel);
    const MeterState &s = state;
    QRectF bar = bar_rect();
    draw_text(p, title_, 0, bar.y() - 1, 10, g_theme.secondary, true);
    p.fillRect(bar, g_theme.grid);

    QString percent = " N/A";
    std::optional<double> f = s.value ? s.range.fraction(*s.value) : std::nullopt;
    if (f) {
        p.fillRect(QRectF(bar.x(), bar.y(), std::max(2.0, bar.width() * *f), bar.height()), g_theme.primary);
        percent = QString::asprintf("%3.0f%%", *f * 100);
    } else {
        // Unavailable is hatched, never an empty (zero) bar.
        p.fillRect(bar, QBrush(alpha(g_theme.secondary, 110), Qt::BDiagPattern));
    }
    p.setPen(pen(g_theme.border, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(bar);

    auto tick = [&](std::optional<double> v, const QColor &color, bool dashed) {
        if (!v) return;
        auto fr = s.range.fraction(*v);
        if (!fr) return;
        double x = bar.x() + bar.width() * *fr;
        p.setPen(pen(color, 1.5, dashed ? Qt::DashLine : Qt::SolidLine));
        p.drawLine(QPointF(x, bar.y() - 3), QPointF(x, bar.bottom() + 3));
    };
    tick(s.start, g_theme.start, false);
    tick(s.target, g_theme.target, true);
    if (s.enabled && s.target) {
        if (auto fr = s.range.fraction(*s.target)) {
            double x = bar.x() + bar.width() * *fr;
            QPolygonF handle({QPointF(x - 4, bar.y() - 6), QPointF(x + 4, bar.y() - 6), QPointF(x, bar.y() - 1)});
            p.setPen(Qt::NoPen);
            p.setBrush(g_theme.target);
            p.drawPolygon(handle);
        }
    }
    draw_text(p, percent, bar.right() + 8, bar.y() - 2, 10, g_theme.text);
    if (s.range.available) {
        draw_text(p, format_value(s.range.min), bar.x(), bar.bottom() + 3, 9, g_theme.secondary);
        draw_text(p, format_value(s.range.max), bar.right(), bar.bottom() + 3, 9, g_theme.secondary, false, true);
    }
    if (hover_ && s.enabled) {
        if (auto fr = s.range.fraction(*hover_)) {
            double x = bar.x() + bar.width() * *fr;
            p.setPen(pen(g_theme.axis, 1));
            p.drawLine(QPointF(x, bar.y() - 2), QPointF(x, bar.bottom() + 2));
            QString text = format_value(*hover_);
            double tw = text_width(text, 9, true);
            double lo = bar.x() + 30, hi = std::max(lo, bar.right() - tw - 30);
            draw_text(p, text, std::clamp(x - tw / 2, lo, hi), bar.bottom() + 3, 9, g_theme.text, true);
        }
    }
}

void MeterView::mouseMoveEvent(QMouseEvent *e) {
    hover_ = value_at(e->position().x());
    if (dragging_ && hover_ && state.on_aim) state.on_aim(*hover_);
    update();
}

void MeterView::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton || !state.enabled) return;
    dragging_ = true;
    if (auto v = value_at(e->position().x()); v && state.on_aim) state.on_aim(*v);
}

void MeterView::mouseReleaseEvent(QMouseEvent *) { dragging_ = false; }

void MeterView::mouseDoubleClickEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton || !state.enabled) return;
    if (auto v = value_at(e->position().x()); v && state.on_aim_and_move) state.on_aim_and_move(*v);
}

void MeterView::leaveEvent(QEvent *) {
    hover_.reset();
    update();
}
