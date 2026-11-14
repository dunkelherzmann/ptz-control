// Position plane and travel meters. Same drawing as the macOS and Windows hosts.
#include "instruments.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

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

// Where the whole range lies: the picture's size, shifted so the target of a move under way, or else the current
// position, is at the picture's centre (the picture itself until there is a position).
QRectF PlaneView::range_rect() const {
    QRectF r = plot_rect();
    const auto &c = state.current;
    std::optional<double> pan, tilt;
    if (state.heading) pan = state.heading->x(), tilt = state.heading->y();
    else if (c && c->pan && c->tilt) pan = c->pan->value, tilt = c->tilt->value;
    if (!pan || !tilt) return r;
    auto fx = state.pan.fraction(*pan), fy = state.tilt.fraction(*tilt);
    if (!fx || !fy) return r;
    return QRectF(r.center().x() - r.width() * *fx, r.center().y() + r.height() * *fy - r.height(), r.width(), r.height());
}

std::optional<QPointF> PlaneView::view_point(double pan, double tilt) const {
    auto fx = state.pan.fraction(pan), fy = state.tilt.fraction(tilt);
    if (!fx || !fy) return std::nullopt;
    QRectF r = range_rect();
    return QPointF(r.x() + r.width() * *fx, r.bottom() - r.height() * *fy);
}

std::optional<QPointF> PlaneView::camera_point(QPointF at) const {
    if (!state.pan.available || !state.tilt.available) return std::nullopt;
    QRectF r = range_rect();
    return QPointF(state.pan.value_at((at.x() - r.x()) / r.width()), state.tilt.value_at((r.bottom() - at.y()) / r.height()));
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

    // Everything on the plane stays inside the picture.
    p.save();
    p.setClipRect(r);
    const QRectF range = range_rect();

    // Beyond the range's edges the camera can't point: shaded.
    QPainterPath outside;
    outside.setFillRule(Qt::OddEvenFill);
    outside.addRect(r);
    outside.addRect(range);
    p.fillPath(outside, QColor(0, 0, 0, 102));

    // The range plane's grid: tenths of the range, its edges, the zero axes when zero is in range, and the limits.
    if (s.show_grid) {
        p.setPen(pen(alpha(white, 46), 0.5));
        for (int i = 1; i <= 9; ++i) {
            double x = range.x() + range.width() * i / 10, y = range.y() + range.height() * i / 10;
            p.drawLine(QPointF(x, range.y()), QPointF(x, range.bottom()));
            p.drawLine(QPointF(range.x(), y), QPointF(range.right(), y));
        }
        p.setPen(pen(alpha(white, 115), 1));
        if (auto zero = view_point(0, s.tilt.min)) p.drawLine(QPointF(zero->x(), range.y()), QPointF(zero->x(), range.bottom()));
        if (auto zero = view_point(s.pan.min, 0)) p.drawLine(QPointF(range.x(), zero->y()), QPointF(range.right(), zero->y()));
        p.setPen(pen(alpha(white, 153), 1));
        p.drawRect(range.adjusted(0.5, 0.5, -0.5, -0.5));
    }
    p.restore();
    if (s.show_grid) {
        // The limits at the range's edges, where they show: pan along the top, tilt down the right side.
        auto width = [](const QString &text) { return QFontMetricsF(mono_font(9, true)).horizontalAdvance(text) + 8; };
        auto visible = [](double edge, double low, double high) { return edge >= low - 1 && edge <= high + 1; };
        const double h = plate_height(9), top = r.y() + 4;
        const QString low = "PAN " + format_value(s.pan.min), high = "PAN " + format_value(s.pan.max);
        if (visible(range.left(), r.left(), r.right()))
            plate(low, QPointF(std::min(range.left() + 4, r.right() - 4 - width(low)), top), white, false, 9);
        if (visible(range.right(), r.left(), r.right()))
            plate(high, QPointF(std::max(range.right() - 4 - width(high), r.left() + 4), top), white, false, 9);
        if (visible(range.top(), r.top(), r.bottom()))
            plate("TILT " + format_value(s.tilt.max),
                  QPointF(r.right() - 4, std::min(std::max(range.top() + 4, r.top() + 24), r.bottom() - 26 - h)), white, true, 9);
        if (visible(range.bottom(), r.top(), r.bottom()))
            plate("TILT " + format_value(s.tilt.min),
                  QPointF(r.right() - 4, std::max(std::min(range.bottom() - 4 - h, r.bottom() - 26 - h), r.top() + 24)), white, true, 9);
    }

    // A dark halo under each marker keeps it readable on any picture.
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

    // Recent measured path; gaps stay gaps.
    p.save();
    p.setClipRect(r);
    QPainterPath trail;
    bool pen_down = false;
    for (const auto &sample : trail_) {
        std::optional<QPointF> point = sample ? view_point(sample->x(), sample->y()) : std::nullopt;
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
        if (auto sp = view_point(s.starting->pan->value, s.starting->tilt->value)) {
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

    // Target: dashed crosshair and ring (an axis left blank keeps the current value), with a dotted line from the
    // current position.
    std::optional<QPointF> current;
    if (s.current && s.current->pan && s.current->tilt) current = view_point(s.current->pan->value, s.current->tilt->value);
    if (s.target_pan || s.target_tilt) {
        std::optional<double> pan = s.target_pan, tilt = s.target_tilt;
        if (!pan && s.current && s.current->pan) pan = s.current->pan->value;
        if (!tilt && s.current && s.current->tilt) tilt = s.current->tilt->value;
        if (pan && tilt) {
            if (auto t = view_point(*pan, *tilt)) {
                QPainterPath cross;
                cross.moveTo(t->x() - 14, t->y());
                cross.lineTo(t->x() + 14, t->y());
                cross.moveTo(t->x(), t->y() - 14);
                cross.lineTo(t->x(), t->y() + 14);
                cross.addEllipse(*t, 8, 8);
                QPen dashed = pen(g_theme.target, 1.5);
                dashed.setDashPattern({4 / 1.5, 2 / 1.5});
                stroke(cross, dashed);
                if (current) {
                    QPainterPath line;
                    line.moveTo(*current);
                    line.lineTo(*t);
                    QPen dotted = pen(alpha(g_theme.target, 178), 1);
                    dotted.setDashPattern({2, 3});
                    stroke(line, dotted);
                }
            }
        }
    }

    // Current measured position: solid reticle.
    if (current) {
        QPainterPath ring;
        ring.addEllipse(*current, 10, 10);
        stroke(ring, pen(g_theme.primary, 1.5));
        QPainterPath dot;
        dot.addEllipse(*current, 4.5, 4.5);
        p.strokePath(dot, QPen(QColor(0, 0, 0, 110), 2));
        p.fillPath(dot, g_theme.primary);
    }
    p.restore();
    if (!current && s.current) {
        plate("POSITION UNAVAILABLE", QPointF(r.right() - 6, r.bottom() - 6 - plate_height()), g_theme.warning, true);
    }

    status();

    // Hover: guides and the pan and tilt under the pointer.
    if (hover_ && s.enabled && r.contains(*hover_)) {
        if (auto camera = camera_point(*hover_)) {
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
    if (!state.enabled) return;
    QRectF r = plot_rect().intersected(range_rect());
    if (r.isEmpty()) return;
    at = QPointF(std::clamp(at.x(), r.left(), r.right()), std::clamp(at.y(), r.top(), r.bottom()));
    auto camera = camera_point(at);
    if (!camera) return;
    if (commit) {
        if (state.on_aim_and_move) state.on_aim_and_move(camera->x(), camera->y());
    } else if (state.on_aim) {
        state.on_aim(camera->x(), camera->y());
    }
}

void PlaneView::mouseMoveEvent(QMouseEvent *e) {
    QPointF at = e->position();
    hover_ = state.enabled && plot_rect().intersected(range_rect()).contains(at) ? std::optional<QPointF>(at) : std::nullopt;
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
