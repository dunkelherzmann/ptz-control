// Position plane and travel meters, drawn with QPainter.
#pragma once

#include "app.h"

#include <QDateTime>
#include <QImage>
#include <QPointF>
#include <QWidget>

#include <vector>

void instruments_set_theme(const Theme &theme);

// The camera's live video, or a still picture of its view, with a view grid on top. The centre is where the camera points, and a point of the
// picture is aimed at in view units (-1..1 from edge to edge, up positive).
struct PlaneState {
    QImage picture;
    QDateTime picture_taken;
    QString live;                // set while the picture is the camera's live video, e.g. "H.264 · 1920 × 1080"
    bool picture_stale = false;  // the camera has moved since the picture was taken
    QString picture_note;        // shown in place of a picture: why there is none
    bool can_aim = false;        // the camera moves within its view, or its view can be measured
    // Where the picture points and how much of the range it spans, once its view is measured: pan and tilt are then
    // drawn over the picture, and a click aims at absolute coordinates.
    struct Overlay {
        double pan, tilt;
        ViewScale scale;
    };
    std::optional<Overlay> overlay;
    bool measuring = false;      // the camera is turning a little and back to measure its view
    std::optional<Snapshot> current, starting;
    std::optional<double> target_pan, target_tilt;
    std::optional<QPointF> picture_target;  // without a measured view: the point aimed at, in view units
    bool enabled = false;
    std::function<void(QPointF)> on_picture_aim, on_picture_aim_and_move;
    std::function<void(double, double)> on_aim, on_aim_and_move;
};

class PlaneView : public QWidget {
public:
    explicit PlaneView(QWidget *parent = nullptr);
    PlaneState state;

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    QRectF picture_rect() const;
    QPointF picture_point(QPointF at) const;  // widget pixels -> view units
    QPointF widget_point(QPointF view) const;  // view units -> widget pixels
    std::optional<QPointF> view_point(double pan, double tilt) const;  // with a measured view
    void aim(QPointF at, bool commit);

    std::optional<QPointF> hover_;  // widget pixels
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
