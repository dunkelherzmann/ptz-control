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

QRectF PlaneView::plot_rect() const {
    return QRectF(46, 10, std::max(1.0, width() - 58.0), std::max(1.0, height() - 34.0));
}

std::optional<QPointF> PlaneView::to_view(const QRectF &r, double pan, double tilt) const {
    auto fx = state.pan.fraction(pan), fy = state.tilt.fraction(tilt);
    if (!fx || !fy) return std::nullopt;
    return QPointF(r.x() + r.width() * *fx, r.bottom() - r.height() * *fy);
}

void PlaneView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), g_theme.panel);
    const PlaneState &s = state;
    QRectF r = plot_rect();
    p.setPen(pen(g_theme.grid, 1));
    for (int i = 0; i <= 10; ++i) {
        double x = r.x() + r.width() * i / 10, y = r.y() + r.height() * i / 10;
        p.drawLine(QPointF(x, r.y()), QPointF(x, r.bottom()));
        p.drawLine(QPointF(r.x(), y), QPointF(r.right(), y));
    }
    p.setPen(pen(g_theme.border, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);

    if (!s.pan.available || !s.tilt.available) {
        QString text = s.enabled ? "PAN/TILT RANGE UNAVAILABLE" : "NO CAMERA LINK";
        double w = text_width(text, 12, true);
        draw_text(p, text, r.x() + (r.width() - w) / 2, r.y() + r.height() / 2 - 8, 12, g_theme.secondary, true);
        return;
    }

    p.setPen(pen(g_theme.axis, 1));
    if (auto zero = to_view(r, 0, s.tilt.min)) p.drawLine(QPointF(zero->x(), r.y()), QPointF(zero->x(), r.bottom()));
    if (auto zero = to_view(r, s.pan.min, 0)) p.drawLine(QPointF(r.x(), zero->y()), QPointF(r.right(), zero->y()));

    double label = 9;
    draw_text(p, format_value(s.pan.min), r.x(), r.bottom() + 4, label, g_theme.secondary);
    draw_text(p, format_value(s.pan.max), r.right(), r.bottom() + 4, label, g_theme.secondary, false, true);
    draw_text(p, "PAN", r.x() + r.width() / 2 - 9, r.bottom() + 4, label, g_theme.secondary);
    draw_text(p, format_value(s.tilt.max), r.x() - 6, r.y() - 2, label, g_theme.secondary, false, true);
    draw_text(p, format_value(s.tilt.min), r.x() - 6, r.bottom() - 12, label, g_theme.secondary, false, true);
    draw_text(p, "TILT", r.x() - 6, r.y() + r.height() / 2 - 6, label, g_theme.secondary, false, true);

    // Measured path; gaps stay gaps.
    p.setPen(pen(alpha(g_theme.primary, 90), 1.5));
    std::optional<QPointF> last;
    for (const auto &sample : trail_) {
        std::optional<QPointF> point = sample ? to_view(r, sample->x(), sample->y()) : std::nullopt;
        if (point && last) p.drawLine(*last, *point);
        last = point;
    }

    if (s.starting && s.starting->pan && s.starting->tilt) {
        if (auto pt = to_view(r, s.starting->pan->value, s.starting->tilt->value)) {
            p.setPen(pen(g_theme.start, 1.5));
            p.drawRect(QRectF(pt->x() - 5, pt->y() - 5, 10, 10));
            draw_text(p, "S", pt->x() + 7, pt->y() - 16, label, g_theme.secondary);
        }
    }

    if (s.target_pan || s.target_tilt) {
        std::optional<double> pan = s.target_pan, tilt = s.target_tilt;
        if (!pan && s.current && s.current->pan) pan = s.current->pan->value;
        if (!tilt && s.current && s.current->tilt) tilt = s.current->tilt->value;
        if (pan && tilt) {
            if (auto pt = to_view(r, *pan, *tilt)) {
                p.setPen(pen(g_theme.target, 1, Qt::DashLine));
                p.drawLine(QPointF(pt->x() - 12, pt->y()), QPointF(pt->x() + 12, pt->y()));
                p.drawLine(QPointF(pt->x(), pt->y() - 12), QPointF(pt->x(), pt->y() + 12));
                p.drawEllipse(*pt, 7, 7);
            }
        }
    }

    if (s.current && s.current->pan && s.current->tilt) {
        if (auto pt = to_view(r, s.current->pan->value, s.current->tilt->value)) {
            p.setPen(Qt::NoPen);
            p.setBrush(g_theme.primary);
            p.drawEllipse(*pt, 4.5, 4.5);
            p.setBrush(Qt::NoBrush);
            p.setPen(pen(g_theme.primary, 1.5));
            p.drawEllipse(*pt, 10, 10);
        }
    } else if (s.current) {
        draw_text(p, "POSITION UNAVAILABLE", r.x() + 6, r.y() + 4, label, g_theme.secondary);
    }

    if (hover_ && s.enabled) {
        QPointF h = *hover_;
        p.setPen(pen(g_theme.axis, 1));
        p.drawLine(QPointF(h.x(), r.y()), QPointF(h.x(), r.bottom()));
        p.drawLine(QPointF(r.x(), h.y()), QPointF(r.right(), h.y()));
        double pan = s.pan.value_at((h.x() - r.x()) / r.width()), tilt = s.tilt.value_at((r.bottom() - h.y()) / r.height());
        QString text = format_value(pan) + ", " + format_value(tilt);
        double w = text_width(text, 10), th = QFontMetricsF(mono_font(10)).height();
        double x = h.x() + 8, y = h.y() - th - 4;
        if (x + w > r.right()) x = h.x() - w - 8;
        if (y < r.y()) y = h.y() + 6;
        draw_text(p, text, x, y, 10, g_theme.text);
    }
}

void PlaneView::aim(QPointF at, bool commit) {
    if (!state.enabled || !state.pan.available || !state.tilt.available) return;
    QRectF r = plot_rect();
    double fx = std::clamp((at.x() - r.x()) / r.width(), 0.0, 1.0), fy = std::clamp((r.bottom() - at.y()) / r.height(), 0.0, 1.0);
    double pan = state.pan.value_at(fx), tilt = state.tilt.value_at(fy);
    if (commit) {
        if (state.on_aim_and_move) state.on_aim_and_move(pan, tilt);
    } else if (state.on_aim) {
        state.on_aim(pan, tilt);
    }
}

void PlaneView::mouseMoveEvent(QMouseEvent *e) {
    QPointF at = e->position();
    hover_ = plot_rect().contains(at) ? std::optional<QPointF>(at) : std::nullopt;
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
