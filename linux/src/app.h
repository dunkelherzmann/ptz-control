// Linux host: Qt 6 Widgets over the shared core. Shared declarations.
#pragma once

#include <QColor>
#include <QFont>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

#include "ptzcore.h"

// ---- text -------------------------------------------------------------------
QString format_value(double value);  // shortest readable form, "." decimal
bool parse_value(const QString &text, double &value);
QFont mono_font(double pixels, bool bold = false);

// ---- theme ------------------------------------------------------------------
enum class Phosphor { System = 0, Green, Amber, Blue, Mono };
QString phosphor_title(Phosphor p);

struct Theme {
    bool dark;
    QColor background, panel, border, grid, axis, primary, secondary, text;
    QColor start, target, warning, good, field, field_text, button, button_text, accent, accent_text;
};
Theme make_theme(Phosphor p);
QColor blend(const QColor &a, const QColor &b, double t);  // t=0 -> a, t=1 -> b

// ---- persisted settings (~/.config/PTZControl/PTZControl.conf) ---------------
struct Settings {
    int route = 0;   // 0 local, 1 ssh
    int vendor = 0;  // 0 onvif, 1 axis
    QString camera_host;
    int camera_port = 80;
    QString camera_user = "admin";
    int https_port = 443;
    int profile = 0;
    double timeout = 15;
    QString ssh_host, ssh_user, ssh_identity;
    int ssh_port = 22;
    int theme = 0;

    static Settings load();
    void save() const;
};

// ---- camera link: one worker thread owns the core session -------------------
struct AxisRange {
    bool available = false;
    double min = 0, max = 0;
    std::optional<double> fraction(double v) const {
        if (!available || max <= min || v < min || v > max) return std::nullopt;
        return (v - min) / (max - min);
    }
    double value_at(double f) const { return min + (max - min) * (f < 0 ? 0 : f > 1 ? 1 : f); }
};

struct Reading {
    double value;
    QString text;
};

struct Snapshot {
    std::optional<Reading> pan, tilt, zoom;
    QString move_status;
    bool empty() const { return !pan && !tilt && !zoom; }
    bool operator==(const Snapshot &o) const;
};

struct CameraInfo {
    QString name, units;
    AxisRange pan, tilt, zoom;
    bool zoom_integer = false;
};

struct LinkResult {
    bool ok = false;
    bool link_lost = false;
    QString error;
};

struct LinkDelegate {
    std::function<void(const QString &)> log;
    std::function<std::optional<QString>(const QString &, bool secret)> prompt;
    std::function<bool(const QString &, const QString &)> confirm;
};

class CameraLink;
CameraLink *create_link();
void destroy_link(CameraLink *link);
void link_set_delegate(CameraLink *link, LinkDelegate delegate);
bool link_busy(CameraLink *link);
void link_connect(CameraLink *link, const Settings &s, const QString &camera_password, const QString &ssh_password,
                  std::function<void(LinkResult, CameraInfo)> done);
void link_cancel_connect(CameraLink *link);
void link_position(CameraLink *link, std::function<void(LinkResult, Snapshot)> done);
void link_move(CameraLink *link, std::optional<double> pan, std::optional<double> tilt, std::optional<double> zoom,
               std::function<void(LinkResult)> done);
void link_stop(CameraLink *link, std::function<void(LinkResult)> done);
void link_web_url(CameraLink *link, std::function<void(LinkResult, QString url, QString notice)> done);
void link_disconnect(CameraLink *link, std::function<void()> done);

// ---- console window ----------------------------------------------------------
class QWidget;
QWidget *create_console();
