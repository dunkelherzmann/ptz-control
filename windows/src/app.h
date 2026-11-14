// Windows host: Win32 + GDI+ over the shared core. Shared declarations.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ptzcore.h"

// ---- text -------------------------------------------------------------------
std::wstring W(const std::string &utf8);
std::string U(const std::wstring &wide);
std::wstring format_value(double value);  // shortest readable form, "." decimal
bool parse_value(const std::wstring &text, double &value);

// ---- theme ------------------------------------------------------------------
enum class Phosphor { System = 0, Green, Amber, Blue, Mono };
const wchar_t *phosphor_title(Phosphor p);

struct Theme {
    bool dark;
    COLORREF background, panel, border, grid, axis, primary, secondary, text;
    COLORREF start, target, warning, good, field, field_text, button, button_text, accent;
};
Theme make_theme(Phosphor p);
COLORREF blend(COLORREF a, COLORREF b, double t);  // t=0 -> a, t=1 -> b

// ---- persisted settings (HKCU\Software\PTZControl, no admin needed) --------
struct Settings {
    int route = 0;   // 0 local, 1 ssh
    int vendor = 0;  // 0 onvif, 1 axis
    std::wstring camera_host;
    int camera_port = 80;
    std::wstring camera_user = L"admin";
    int https_port = 443;
    int profile = 0;
    double timeout = 15;
    std::wstring ssh_host, ssh_user, ssh_identity;
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
    std::wstring text;
};

struct Snapshot {
    std::optional<Reading> pan, tilt, zoom;
    std::wstring move_status;
    bool empty() const { return !pan && !tilt && !zoom; }
    bool operator==(const Snapshot &o) const;
};

struct CameraInfo {
    std::wstring name, units;
    AxisRange pan, tilt, zoom;
    bool zoom_integer = false;
    bool view_moves = false;  // a point of the picture can be brought to its centre
};

struct LinkResult {
    bool ok = false;
    bool link_lost = false;
    std::wstring error;
};

constexpr UINT WM_APP_CALL = WM_APP + 1;  // lParam: std::function<void()>*, run then delete
constexpr UINT WM_APP_SYNC = WM_APP + 2;  // lParam: std::function<void()>*, run (sender waits)

class CameraLink;
CameraLink *create_link(HWND notify);
void destroy_link(CameraLink *link);

struct LinkDelegate {
    std::function<void(const std::wstring &)> log;
    std::function<std::optional<std::wstring>(const std::wstring &, bool secret)> prompt;
    std::function<bool(const std::wstring &, const std::wstring &)> confirm;
};

void link_set_delegate(CameraLink *link, LinkDelegate delegate);
bool link_busy(CameraLink *link);
void link_connect(CameraLink *link, const Settings &s, const std::wstring &camera_password,
                  const std::wstring &ssh_password, std::function<void(LinkResult, CameraInfo)> done);
void link_cancel_connect(CameraLink *link);
void link_position(CameraLink *link, std::function<void(LinkResult, Snapshot)> done);
void link_move(CameraLink *link, std::optional<double> pan, std::optional<double> tilt, std::optional<double> zoom,
               std::function<void(LinkResult)> done);
void link_stop(CameraLink *link, std::function<void(LinkResult)> done);
// A still picture of the camera's current view (JPEG or PNG bytes).
void link_picture(CameraLink *link, std::function<void(LinkResult, std::string bytes)> done);
// Brings the point (x, y) of the last picture to its centre; x and y run from -1 to 1, up positive.
void link_move_in_view(CameraLink *link, double x, double y, std::function<void(LinkResult)> done);
// The camera's live video: `picture` gets the newest decoded frame and `ended` the reason it stopped by itself or
// can't be decoded, both on the UI thread; `done` gets a description such as "H.264 over RTSP".
struct LiveFrame {
    std::wstring codec;  // "H.264" or "H.265"
    int width = 0, height = 0;
    std::vector<unsigned char> bgra;  // top-down rows, 4 bytes per pixel
};
void link_start_live(CameraLink *link, std::function<void(std::shared_ptr<LiveFrame>)> picture,
                     std::function<void(const std::wstring &)> ended, std::function<void(LinkResult, std::wstring)> done);
// Stops the live video, if any, and waits for its thread.
void link_stop_live(CameraLink *link);
void link_web_url(CameraLink *link, std::function<void(LinkResult, std::wstring url, std::wstring notice)> done);
void link_disconnect(CameraLink *link, std::function<void()> done);

// ---- live video decoding (live.cpp, Media Foundation) --------------------------------
class LiveDecoder;
// `post` runs a call on the UI thread; `picture` (the newest frame only) and `failed` run there.
std::shared_ptr<LiveDecoder> live_decoder_create(std::function<void(std::function<void()>)> post,
                                                 std::function<void(std::shared_ptr<LiveFrame>)> picture,
                                                 std::function<void(const std::wstring &)> failed);
void live_decoder_decode(LiveDecoder &decoder, const ptz_frame &frame);  // the stream's thread
void live_decoder_close(LiveDecoder &decoder);                           // once the stream has stopped

// ---- custom instrument windows ----------------------------------------------
void register_instrument_classes(HINSTANCE instance);
HWND create_plane(HWND parent, int id);
HWND create_meter(HWND parent, int id, const wchar_t *title);

namespace Gdiplus { class Bitmap; }
// The camera's live video, or a still picture of its view, with a view grid on top. The centre is where the camera
// points, and a point of the picture is aimed at in view units (-1..1 from edge to edge, up positive).
struct PlaneState {
    std::shared_ptr<Gdiplus::Bitmap> picture;
    int picture_width = 0, picture_height = 0;
    std::wstring picture_taken;  // HH:MM:SS
    bool picture_stale = false;  // the camera has moved since the picture was taken
    std::wstring picture_note;   // shown in place of a picture: why there is none
    std::wstring live;           // set while the picture is the camera's live video, e.g. "H.264 · 1920 × 1080"
    bool view_moves = false;
    std::optional<std::pair<float, float>> picture_target;  // view units
    bool enabled = false;
    std::function<void(float, float)> on_picture_aim, on_picture_aim_and_move;
};
PlaneState &plane_state(HWND plane);
bool plane_set_picture(HWND plane, const std::string &bytes);  // JPEG or PNG; false when it can't be read
bool plane_set_frame(HWND plane, const LiveFrame &frame);       // a decoded frame of the live video

struct MeterState {
    AxisRange range;
    std::optional<double> value, start, target;
    bool enabled = false, integer = false;
    std::function<void(double)> on_aim, on_aim_and_move;
};
MeterState &meter_state(HWND meter);

void instruments_set_theme(const Theme &theme, UINT dpi);

// ---- saved logins (spec §17; logins.cpp) ----------------------------------------
enum class LoginStore { Ssh = 0, Camera = 1 };
enum class VaultState { Unavailable, New, Plain, Locked, Unlocked };

struct SavedLogin {  // a name that tells it apart, a username and its password
    std::wstring name, user, password;
};
// Save login: asks what to call the login, suggesting `suggested`; empty when not saved.
std::wstring vault_ask_name(HWND owner, LoginStore store, const std::wstring &user, const std::wstring &suggested);

VaultState vault_state();
std::wstring vault_open_error();
std::vector<SavedLogin> vault_logins(LoginStore store);
bool vault_save(const SavedLogin &login, LoginStore store, int replacing, std::wstring *error, int *index = nullptr);
bool vault_remove(int index, LoginStore store, std::wstring *error);
void vault_lock();
void vault_subscribe(std::function<void()> fn);  // called after every change
bool vault_unlock_dialog(HWND owner);
bool vault_change_admin_dialog(HWND owner);
bool vault_ensure_readable(HWND owner);
bool vault_ensure_writable(HWND owner);  // asks how to keep logins the first time
// Modal. From a Login menu, `store` picks the list shown; otherwise it shows the last one.
void show_manage_logins(HWND owner, std::optional<LoginStore> store = std::nullopt);
HWND manage_logins_for_snapshot(HWND owner, LoginStore store);  // test hook: built, not shown
void manage_logins_close_snapshot();
HWND create_tooltips(HWND owner);
HWND add_tooltip(HWND tips, HWND control, const wchar_t *text);

// ---- console window ----------------------------------------------------------
HWND create_console(HINSTANCE instance, int show);
bool console_translate(MSG &msg);
