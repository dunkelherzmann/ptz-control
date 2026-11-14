// Position plane and travel meters, drawn with QPainter.
#pragma once

#include "app.h"

#include <QDateTime>
#include <QImage>
#include <QPointF>
#include <QWidget>

#include <vector>

void instruments_set_theme(const Theme &theme);

// The camera's live video, or a still picture of its view, with the pan/tilt range plane on top, the size of the
// picture (pan across, tilt up) and centred on where the camera points: the current position stays at the picture's
// centre as a crosshair, and the grid, zero axes, range edges and limits (hidden with Show Grid off), the trail, the
// starting position and the target move under it. Click or drag to aim at a pan and tilt; double-click to aim and
// move. Only measured positions are drawn: no reading, no marker.
struct PlaneState {
    QImage picture;
    QDateTime picture_taken;
    QString live;                // set while the picture is the camera's live video, e.g. "H.264 · 1920 × 1080"
    bool picture_stale = false;  // the camera has moved since the picture was taken
    QString picture_note;        // shown in place of a picture: why there is none
    bool show_grid = true;
    AxisRange pan, tilt;
    std::optional<Snapshot> current, starting;
    std::optional<double> target_pan, target_tilt;
    // The target of a move under way: the plane is centred on it until the camera stops, so it doesn't wait for the
    // readings; the crosshair still shows the readings, closing in on the centre.
    std::optional<QPointF> heading;  // pan, tilt
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
    QRectF plot_rect() const;   // the picture's frame
    QRectF range_rect() const;  // where the whole range lies: the picture's size, centred on the heading or current position
    std::optional<QPointF> view_point(double pan, double tilt) const;    // camera units -> widget pixels
    std::optional<QPointF> camera_point(QPointF at) const;               // widget pixels -> pan, tilt
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
