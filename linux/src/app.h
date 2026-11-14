// Linux host: Qt 6 Widgets over the shared core. Shared declarations.
#pragma once

#include <QColor>
#include <QFont>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>
#include <vector>

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
    bool view_moves = false;  // a point of the picture can be brought to its centre
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
class QImage;
// A still picture of the camera's current view.
void link_picture(CameraLink *link, std::function<void(LinkResult, QImage)> done);
// Brings the point (x, y) of the last picture to its centre; x and y run from -1 to 1, up positive.
void link_move_in_view(CameraLink *link, double x, double y, std::function<void(LinkResult)> done);
// The camera's live video (live.cpp decodes it). Pictures and `ended` (it stopped by itself or can't be decoded)
// arrive on the UI thread; `done` gets a description such as "H.264 over RTSP".
void link_live_start(CameraLink *link, std::function<void(QImage)> picture, std::function<void(QString)> ended,
                     std::function<void(LinkResult, QString description)> done);
// Stops the live video, if any, and waits for its thread.
void link_live_stop(CameraLink *link, std::function<void()> done = nullptr);
void link_web_url(CameraLink *link, std::function<void(LinkResult, QString url, QString notice)> done);
void link_disconnect(CameraLink *link, std::function<void()> done);

// live.cpp: one stream and its decoder. `post` runs a function on the UI thread.
using LivePost = std::function<void(std::function<void()>)>;
class LiveVideo;
LiveVideo *live_start(ptz_session *session, LivePost post, std::function<void(QImage)> picture,
                      std::function<void(QString)> ended, QString *description, QString *error);
void live_stop(LiveVideo *live);  // from any thread but the stream's own; before ptz_close

// ---- saved logins (spec §17; vault.cpp) ----------------------------------------
enum class LoginStore { Ssh = 0, Camera = 1 };
enum class VaultState { Unavailable, New, Plain, Locked, Unlocked };

struct SavedLogin {  // a name that tells it apart, a username and its password
    QString name, user, password;
};
// Save login: asks what to call the login, suggesting `suggested`; empty when not saved.
QString vault_ask_name(QWidget *parent, LoginStore store, const QString &user, const QString &suggested);

class QPixmap;
class QWidget;
VaultState vault_state();
QString vault_open_error();
std::vector<SavedLogin> vault_logins(LoginStore store);
bool vault_save(const SavedLogin &login, LoginStore store, int replacing, QString *error, int *index = nullptr);
bool vault_remove(int index, LoginStore store, QString *error);
void vault_lock();
void vault_subscribe(QObject *owner, std::function<void()> fn);  // called after every change while owner lives
bool vault_unlock_dialog(QWidget *parent);
bool vault_change_admin_dialog(QWidget *parent);
bool vault_ensure_readable(QWidget *parent);
bool vault_ensure_writable(QWidget *parent);  // asks how to keep logins the first time
// From a Login menu, `store` picks the list shown; otherwise it keeps the last one.
void show_manage_logins(QWidget *parent, std::optional<LoginStore> store = std::nullopt);
QPixmap manage_logins_snapshot(QWidget *parent, LoginStore store);  // test hook
QPixmap login_form_snapshot(QWidget *parent, LoginStore store);     // test hook
class QDialog;
QDialog *open_manage_logins_for_test(QWidget *parent);              // test hook: built, not shown

// ---- console window ----------------------------------------------------------
class QWidget;
QWidget *create_console();
// The ☰ Main Menu (spec §13 H1) at the end of `row`, and the app's shortcuts in `window`.
class QBoxLayout;
void attach_main_menu(QWidget *window, QBoxLayout *row);
