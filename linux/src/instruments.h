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
    bool view_moves = false;
    std::optional<QPointF> picture_target;
    bool enabled = false;
    std::function<void(QPointF)> on_picture_aim, on_picture_aim_and_move;
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
