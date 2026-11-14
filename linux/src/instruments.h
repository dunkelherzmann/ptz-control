// Position plane and travel meters, drawn with QPainter.
#pragma once

#include "app.h"

#include <QDateTime>
#include <QImage>
#include <QPointF>
#include <QWidget>

#include <vector>

class QPainter;
void instruments_set_theme(const Theme &theme);

// What the position plane shows: the camera's whole pan/tilt range, or a still picture of its view.
enum class PlaneMode { Range, Picture };

struct PlaneState {
    PlaneMode mode = PlaneMode::Range;
    // Picture mode: the camera's still picture with a view grid on top. The centre is where the camera points, and a
    // point of the picture is aimed at in view units (-1..1 from edge to edge, up positive).
    QImage picture;
    QDateTime picture_taken;
    bool picture_stale = false;  // the camera has moved since the picture was taken
    QString picture_note;        // shown in place of a picture: why there is none
    bool view_moves = false;
    std::optional<QPointF> picture_target;
    std::function<void(QPointF)> on_picture_aim, on_picture_aim_and_move;

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
    QRectF picture_rect() const;
    QPointF picture_point(QPointF at) const;  // widget pixels -> view units
    void paint_picture(QPainter &p);
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
