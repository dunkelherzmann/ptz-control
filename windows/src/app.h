// Windows host: Win32 + GDI+ over the shared core. Shared declarations.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <optional>
#include <string>

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
void link_web_url(CameraLink *link, std::function<void(LinkResult, std::wstring url, std::wstring notice)> done);
void link_disconnect(CameraLink *link, std::function<void()> done);

// ---- custom instrument windows ----------------------------------------------
void register_instrument_classes(HINSTANCE instance);
HWND create_plane(HWND parent, int id);
HWND create_meter(HWND parent, int id, const wchar_t *title);

struct PlaneState {
    AxisRange pan, tilt;
    std::optional<Snapshot> current, starting;
    std::optional<double> target_pan, target_tilt;
    bool enabled = false;
    std::function<void(double, double)> on_aim, on_aim_and_move;
};
PlaneState &plane_state(HWND plane);
void plane_reset_trail(HWND plane);
void plane_record(HWND plane);  // append current measurement (or a gap) to the trail

struct MeterState {
    AxisRange range;
    std::optional<double> value, start, target;
    bool enabled = false, integer = false;
    std::function<void(double)> on_aim, on_aim_and_move;
};
MeterState &meter_state(HWND meter);

void instruments_set_theme(const Theme &theme, UINT dpi);

// ---- console window ----------------------------------------------------------
HWND create_console(HINSTANCE instance, int show);
bool console_translate(MSG &msg);
