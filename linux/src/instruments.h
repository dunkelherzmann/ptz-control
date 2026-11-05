// Position plane and travel meters, drawn with QPainter.
#pragma once

#include "app.h"

#include <QPointF>
#include <QWidget>

#include <vector>

void instruments_set_theme(const Theme &theme);

struct PlaneState {
    AxisRange pan, tilt;
    std::optional<Snapshot> current, starting;
    std::optional<double> target_pan, target_tilt;
    bool enabled = false;
    std::function<void(double, double)> on_aim, on_aim_and_move;
};

class PlaneView : public QWidget {
public:
    explicit PlaneView(QWidget *parent = nullptr);
    PlaneState state;
    void reset_trail() { trail_.clear(); }
    void record();  // append the current measurement (or a gap) to the trail

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    QRectF plot_rect() const;
    std::optional<QPointF> to_view(const QRectF &r, double pan, double tilt) const;
    void aim(QPointF at, bool commit);

    std::vector<std::optional<QPointF>> trail_;  // camera units; nullopt marks a gap
    std::optional<QPointF> hover_;               // widget pixels
    bool dragging_ = false;
};

struct MeterState {
    AxisRange range;
    std::optional<double> value, start, target;
    bool enabled = false, integer = false;
    std::function<void(double)> on_aim, on_aim_and_move;
};

class MeterView : public QWidget {
public:
    MeterView(const QString &title, QWidget *parent = nullptr);
    MeterState state;
    void refresh_cursor();

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    QRectF bar_rect() const;
    std::optional<double> value_at(double x) const;

    QString title_;
    std::optional<double> hover_;
    bool dragging_ = false;
};
