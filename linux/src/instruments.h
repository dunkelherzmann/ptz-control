// Position plane and travel meters, drawn with QPainter.
#pragma once

#include "app.h"

#include <QDateTime>
#include <QImage>
#include <QPointF>
#include <QWidget>

#include <vector>

void instruments_set_theme(const Theme &theme);

// The camera's live video, or a still picture of its view, with the pan/tilt plane drawn over it at the picture's own
// scale: once the view is measured at the current zoom, every point of the picture is the pan and tilt it shows, so
// the grid, the zero axes, the range's limits, the trail, the starting position and the target sit on what they point
// at, and the current position is the picture's centre. Click or drag to aim at the pan and tilt under the pointer;
// double-click to aim and move, which brings that point to the centre. Until the view is measured, a double-click
// measures it first. Nothing covers the picture but thin lines and small labels.
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
    bool degrees = false;              // pan and tilt are angles in degrees: the plane is projected as the lens sees it
    std::optional<ViewScale> scale;    // how wide the picture is at the current zoom; none until measured
    bool measuring = false;            // the camera is turning a little and back to measure its view
    bool enabled = false;
    std::function<void(double, double)> on_aim, on_aim_and_move;
    std::function<void(QPointF)> on_measure_and_move;  // a double-click before the view is measured: -1…1, up positive
};

// One of the plane's lines from the core, in picture points (-1…1, up positive).
struct PlaneLine {
    enum Kind { Grid, Zero, Limit } kind;
    bool pan;
    QString label;
    bool labelled;
    QPointF label_at;
    std::vector<QPointF> points;
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
    QRectF plot_rect() const;  // the picture's frame
    std::optional<ptz_view> view() const;  // where the camera points and how wide the picture is, once both are known
    std::optional<QPointF> locate(double pan, double tilt, const QRectF &r) const;  // camera units -> widget pixels
    std::optional<QPointF> camera_point(QPointF at, const QRectF &r) const;         // widget pixels -> pan, tilt
    const std::vector<PlaneLine> &lines();
    void aim(QPointF at, bool commit);

    std::vector<std::optional<QPointF>> trail_;  // camera units; nullopt marks a gap
    std::optional<QPointF> hover_;               // widget pixels
    bool dragging_ = false;
    std::vector<double> lines_key_;
    std::vector<PlaneLine> lines_;
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
