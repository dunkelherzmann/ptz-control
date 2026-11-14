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

// The picture's frame: as large as fits, in the picture's shape (16:9 until there is one), centred.
QRectF PlaneView::picture_rect() const {
    QRectF area = QRectF(rect()).adjusted(1, 1, -1, -1);
    double pw = state.picture.isNull() ? 16 : state.picture.width(), ph = state.picture.isNull() ? 9 : state.picture.height();
    if (pw <= 0 || ph <= 0 || area.width() <= 0 || area.height() <= 0) return area;
    double scale = std::min(area.width() / pw, area.height() / ph), w = std::round(pw * scale), h = std::round(ph * scale);
    return QRectF(std::round(area.center().x() - w / 2), std::round(area.center().y() - h / 2), w, h);
}

QPointF PlaneView::picture_point(QPointF at) const {
    QRectF r = picture_rect();
    return QPointF((at.x() - r.x()) / r.width() * 2 - 1, 1 - (at.y() - r.y()) / r.height() * 2);
}

void PlaneView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), g_theme.panel);
    const PlaneState &s = state;
    QRectF r = picture_rect();
    const QColor white(255, 255, 255);
    auto centred = [&](const QString &text, double y, double size, bool bold, const QColor &color) {
        draw_text(p, text, r.center().x() - text_width(text, size, bold) / 2, y, size, color, bold);
    };
    // A label on a dark plate, readable on any picture. `at` is the plate's top-left (or top-right) corner.
    auto plate = [&](const QString &text, QPointF at, const QColor &color, bool align_right = false) {
        QFontMetricsF m(mono_font(10, true));
        double w = m.horizontalAdvance(text) + 8, h = m.height() + 4;
        if (align_right) at.rx() -= w;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 140));
        p.drawRoundedRect(QRectF(at.x(), at.y(), w, h), 3, 3);
        p.setBrush(Qt::NoBrush);
        draw_text(p, text, at.x() + 4, at.y() + 2, 10, color, true);
        return h;
    };

    p.fillRect(r, Qt::black);
    if (s.picture.isNull()) {
        p.setPen(pen(g_theme.grid, 1));
        for (int i = 0; i <= 10; ++i) {
            double x = r.x() + r.width() * i / 10, y = r.y() + r.height() * i / 10;
            p.drawLine(QPointF(x, r.y()), QPointF(x, r.bottom()));
            p.drawLine(QPointF(r.x(), y), QPointF(r.right(), y));
        }
        QString title = !s.enabled ? "NO CAMERA LINK" : s.picture_note.isEmpty() ? "TAKING A PICTURE…" : "NO PICTURE";
        double th = QFontMetricsF(mono_font(12, true)).height();
        centred(title, r.center().y() - th - 2, 12, true, g_theme.secondary);
        if (s.enabled && !s.picture_note.isEmpty()) {
            p.setFont(QFont());
            QFont f = p.font();
            f.setPixelSize(11);
            p.setFont(f);
            p.setPen(g_theme.secondary);
            p.drawText(QRectF(r.x() + 24, r.center().y() + 4, r.width() - 48, 40), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                       s.picture_note);
        }
        return;
    }
    p.save();
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setOpacity(s.picture_stale ? 0.45 : 1);
    p.drawImage(r, s.picture);
    p.restore();

    // View grid: tenths of the picture, the centre lines stronger.
    p.setPen(pen(alpha(white, 41), 0.5));
    for (int i = 1; i <= 9; ++i) {
        if (i == 5) continue;
        double x = r.x() + r.width() * i / 10, y = r.y() + r.height() * i / 10;
        p.drawLine(QPointF(x, r.y()), QPointF(x, r.bottom()));
        p.drawLine(QPointF(r.x(), y), QPointF(r.right(), y));
    }
    QPointF c = r.center();
    p.setPen(pen(alpha(white, 77), 0.75));
    p.drawLine(QPointF(c.x(), r.y()), QPointF(c.x(), r.bottom()));
    p.drawLine(QPointF(r.x(), c.y()), QPointF(r.right(), c.y()));

    // Where the camera points: the picture's centre.
    p.setPen(pen(g_theme.primary, 1.5));
    p.drawEllipse(c, 10, 10);
    p.setPen(Qt::NoPen);
    p.setBrush(g_theme.primary);
    p.drawEllipse(c, 3, 3);
    p.setBrush(Qt::NoBrush);

    // Target: dashed crosshair, with a line from the centre.
    if (s.picture_target) {
        QPointF t(r.x() + (s.picture_target->x() + 1) / 2 * r.width(), r.y() + (1 - s.picture_target->y()) / 2 * r.height());
        QPen dashed = pen(g_theme.target, 1.5);
        dashed.setDashPattern({4 / 1.5, 2 / 1.5});
        p.setPen(dashed);
        p.drawLine(QPointF(t.x() - 14, t.y()), QPointF(t.x() + 14, t.y()));
        p.drawLine(QPointF(t.x(), t.y() - 14), QPointF(t.x(), t.y() + 14));
        p.drawEllipse(t, 8, 8);
        QPen dotted = pen(alpha(g_theme.target, 178), 1);
        dotted.setDashPattern({2, 3});
        p.setPen(dotted);
        p.drawLine(c, t);
    }

    // Status plates: when the picture was taken, or that it is out of date.
    QString status = s.picture_stale ? "MOVING · NEW PICTURE WHEN IT STOPS"
                     : s.picture_taken.isValid() ? "PICTURE " + s.picture_taken.toString("HH:mm:ss") : QString("PICTURE");
    double plate_h = QFontMetricsF(mono_font(10, true)).height() + 4;
    plate(status, QPointF(r.x() + 6, r.bottom() - 6 - plate_h), s.picture_stale ? g_theme.warning : white);
    if (!s.view_moves)
        plate("THIS CAMERA CAN'T AIM FROM ITS PICTURE", QPointF(r.right() - 6, r.bottom() - 6 - plate_h), g_theme.warning, true);

    // Hover: guides and the point in view units.
    if (hover_ && s.view_moves && r.contains(*hover_)) {
        QPointF h = *hover_;
        p.setPen(pen(alpha(white, 89), 0.5));
        p.drawLine(QPointF(h.x(), r.y()), QPointF(h.x(), r.bottom()));
        p.drawLine(QPointF(r.x(), h.y()), QPointF(r.right(), h.y()));
        QPointF v = picture_point(h);
        QString text = QString::asprintf("%s %.2f  %s %.2f", v.x() < 0 ? "◀" : "▶", std::abs(v.x()), v.y() < 0 ? "▼" : "▲",
                                         std::abs(v.y()));
        QPointF at(h.x() + 10, h.y() - 8 - plate_h);
        if (at.x() + 110 > r.right()) at.setX(h.x() - 120);
        if (at.y() < r.y()) at.setY(h.y() + 8);
        plate(text, at, white);
    }
}

void PlaneView::aim(QPointF at, bool commit) {
    if (!state.enabled || state.picture.isNull() || !state.view_moves || !picture_rect().contains(at)) return;
    QPointF point = picture_point(at);
    if (commit) {
        if (state.on_picture_aim_and_move) state.on_picture_aim_and_move(point);
    } else if (state.on_picture_aim) {
        state.on_picture_aim(point);
    }
}

void PlaneView::mouseMoveEvent(QMouseEvent *e) {
    QPointF at = e->position();
    hover_ = picture_rect().contains(at) ? std::optional<QPointF>(at) : std::nullopt;
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
