// The console window: link configuration, live instruments, target entry and
// the event buffer. Logic mirrors the macOS ConsoleWindowController.
#include "app.h"

#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <richedit.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

using namespace Gdiplus;

namespace {

enum : int {
    ID_ROUTE_LOCAL = 100, ID_ROUTE_SSH, ID_VENDOR_ONVIF, ID_VENDOR_AXIS, ID_CONNECT, ID_MOVE, ID_START, ID_STOP, ID_WEB,
    ID_MENU, ID_REFRESH_PICTURE,
    ID_SSH_HOST = 200, ID_SSH_USER, ID_SSH_PASSWORD, ID_SSH_PORT, ID_SSH_KEY, ID_CAM_HOST, ID_CAM_PORT, ID_CAM_USER,
    ID_CAM_PASSWORD, ID_HTTPS_PORT, ID_PROFILE, ID_TIMEOUT, ID_TARGET_PAN, ID_TARGET_TILT, ID_TARGET_ZOOM, ID_LOG,
    ID_PLANE, ID_METER_PAN, ID_METER_TILT, ID_METER_ZOOM,
    ID_SSH_LOGIN = 260, ID_CAM_LOGIN, ID_SAVE_SSH, ID_SAVE_CAM,
    CMD_CONNECT = 400, CMD_DISCONNECT, CMD_MOVE, CMD_RETURN, CMD_STOP, CMD_REFRESH, CMD_PAUSE, CMD_WEB, CMD_HELP, CMD_ABOUT,
    CMD_LOGINS, CMD_QUIT, CMD_UNDO, CMD_REDO, CMD_CUT, CMD_COPY, CMD_PASTE, CMD_SELECT_ALL, CMD_MINIMIZE, CMD_MAXIMIZE,
    CMD_CLOSE, CMD_REFRESH_PICTURE,
    CMD_THEME = 450,  // + Phosphor index; keep clear of the commands above
};

enum class State { Idle, Connecting, Connected, Disconnecting };

enum RowKind { kEdit = 0, kCombo = 1, kCheck = 2 };

struct Row {
    std::wstring label;
    HWND edit;
    RECT frame;
    bool ssh;
    int kind = kEdit;
};

struct Console {
    HWND hwnd = nullptr;
    HINSTANCE instance = nullptr;
    UINT dpi = 96;
    Phosphor phosphor = Phosphor::System;
    Theme theme = make_theme(Phosphor::System);
    Settings settings;
    CameraLink *link = nullptr;
    State state = State::Idle;
    std::optional<CameraInfo> info;
    std::optional<Snapshot> starting, current;
    int failures = 0;
    bool paused = false;
    ULONGLONG next_refresh = 0;
    bool web_pending = false;
    bool closing = false;
    bool taking_picture = false;
    ULONGLONG picture_due = 0, last_read = 0;  // when the camera last moved (a fresh picture is due), the last reading
    int route = 0, vendor = 0;
    std::wstring message;
    bool message_error = false;

    HWND route_local, route_ssh, vendor_onvif, vendor_axis, connect, move, start, stop, web, menu, refresh_picture;
    HWND ssh_host, ssh_user, ssh_password, ssh_port, ssh_key, cam_host, cam_port, cam_user, cam_password;
    HWND https_port, profile, timeout, target_pan, target_tilt, target_zoom, log, plane, meter_pan, meter_tilt,
        meter_zoom;
    std::vector<Row> rows;
    HWND ssh_login = nullptr, cam_login = nullptr, save_ssh = nullptr, save_cam = nullptr, tips = nullptr;
    bool save_ssh_on = false, save_cam_on = false, filling = false;
    std::vector<HWND> target_fields;

    HFONT field_font = nullptr, log_font = nullptr, button_font = nullptr;
    HBRUSH field_brush = nullptr;
    HACCEL accelerators = nullptr;

    RECT sidebar{}, header{}, plane_panel{}, readout_panel{}, meters_panel{}, target_panel{}, log_panel{}, footer{};
    RECT chip{}, message_rc{}, vendor_note{}, target_note{};
    struct Caption {
        int x, y;
        std::wstring text;
    };
    std::vector<Caption> captions;
    std::vector<RECT> target_frames;
};

Console *g = nullptr;

int S(int v) { return MulDiv(v, static_cast<int>(g->dpi), 96); }
float SF(float v) { return v * static_cast<float>(g->dpi) / 96.0f; }
Color C(COLORREF c, BYTE a = 255) { return Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

const wchar_t *mono_family() {
    static const wchar_t *family = [] {
        FontFamily cascadia(L"Cascadia Mono");
        return cascadia.IsAvailable() ? L"Cascadia Mono" : L"Consolas";
    }();
    return family;
}

std::wstring text_of(HWND edit) {
    int n = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(edit, text.data(), n + 1);
    text.resize(static_cast<size_t>(n));
    return text;
}

std::wstring trimmed(HWND edit) {
    std::wstring t = text_of(edit);
    size_t a = t.find_first_not_of(L" \t"), b = t.find_last_not_of(L" \t");
    return a == std::wstring::npos ? L"" : t.substr(a, b - a + 1);
}

void wipe(HWND edit) {
    std::wstring t = text_of(edit);
    SecureZeroMemory(t.data(), t.size() * sizeof(wchar_t));
}

// ---- event buffer -----------------------------------------------------------------

void log_line(const std::wstring &line, bool warning = false) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t stamp[16];
    swprintf(stamp, 16, L"%02d:%02d:%02d  ", now.wHour, now.wMinute, now.wSecond);
    std::wstring text = std::wstring(stamp) + line + L"\r\n";
    HWND log = g->log;
    SendMessageW(log, EM_SETREADONLY, FALSE, 0);
    if (SendMessageW(log, EM_GETLINECOUNT, 0, 0) > 500) {
        LRESULT second = SendMessageW(log, EM_LINEINDEX, 1, 0);
        CHARRANGE head{0, static_cast<LONG>(second)};
        SendMessageW(log, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&head));
        SendMessageW(log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
    }
    CHARRANGE end{-1, -1};
    SendMessageW(log, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&end));
    CHARFORMAT2W format{};
    format.cbSize = sizeof format;
    format.dwMask = CFM_COLOR;
    format.crTextColor = warning ? g->theme.warning : g->theme.text;
    SendMessageW(log, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
    SendMessageW(log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(log, EM_SETREADONLY, TRUE, 0);
    SendMessageW(log, WM_VSCROLL, SB_BOTTOM, 0);
}

// ---- modal prompt (SSH password / passphrase / MFA) ---------------------------------

struct PromptState {
    HWND edit = nullptr;
    bool done = false, accepted = false;
    std::wstring answer;
};

LRESULT CALLBACK prompt_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto *p = reinterpret_cast<PromptState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
                p->accepted = LOWORD(wp) == IDOK;
                if (p->accepted) p->answer = text_of(p->edit);
                SetWindowTextW(p->edit, L"");
                p->done = true;
                return 0;
            }
            break;
        case WM_CLOSE:
            p->done = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

std::optional<std::wstring> run_prompt(const std::wstring &message, bool secret) {
    static bool registered = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = prompt_proc;
        wc.hInstance = g->instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = L"PTZPrompt";
        wc.hIcon = LoadIconW(g->instance, MAKEINTRESOURCEW(1));
        return RegisterClassW(&wc) != 0;
    }();
    (void)registered;
    PromptState state;
    int w = S(420), h = S(190);
    RECT owner;
    GetWindowRect(g->hwnd, &owner);
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"PTZPrompt", L"SSH needs your input",
                                  WS_POPUP | WS_CAPTION | WS_SYSMENU, owner.left + (owner.right - owner.left - w) / 2,
                                  owner.top + (owner.bottom - owner.top - h) / 3, w, h, g->hwnd, nullptr, g->instance, nullptr);
    SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof metrics;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0, g->dpi);
    HFONT font = CreateFontIndirectW(&metrics.lfMessageFont);
    RECT client;
    GetClientRect(dialog, &client);
    HWND label = CreateWindowExW(0, L"STATIC", message.c_str(), WS_CHILD | WS_VISIBLE | SS_EDITCONTROL, S(16), S(14),
                                 client.right - S(32), S(56), dialog, nullptr, g->instance, nullptr);
    state.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | (secret ? ES_PASSWORD : 0), S(16),
                                 S(76), client.right - S(32), S(26), dialog, nullptr, g->instance, nullptr);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              client.right - S(196), client.bottom - S(44), S(84), S(28), dialog,
                              reinterpret_cast<HMENU>(IDOK), g->instance, nullptr);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, client.right - S(100),
                                  client.bottom - S(44), S(84), S(28), dialog, reinterpret_cast<HMENU>(IDCANCEL),
                                  g->instance, nullptr);
    for (HWND c : {label, state.edit, ok, cancel}) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    EnableWindow(g->hwnd, FALSE);
    ShowWindow(dialog, SW_SHOW);
    SetForegroundWindow(dialog);
    SetFocus(state.edit);
    MSG msg;
    while (!state.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(dialog, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(g->hwnd, TRUE);
    DestroyWindow(dialog);
    DeleteObject(font);
    SetForegroundWindow(g->hwnd);
    if (!state.accepted) return std::nullopt;
    std::wstring answer = state.answer;
    SecureZeroMemory(state.answer.data(), state.answer.size() * sizeof(wchar_t));
    return answer;
}

// ---- state -> views -----------------------------------------------------------------

std::wstring describe(const Snapshot &s) {
    auto v = [](const std::optional<Reading> &r) { return r ? r->text : std::wstring(L"unknown"); };
    std::wstring text = L"Pan:" + v(s.pan) + L" Tilt:" + v(s.tilt) + L" Zoom:" + v(s.zoom);
    if (!s.move_status.empty()) text += L" (" + s.move_status + L")";
    return text;
}

struct Target {
    std::optional<double> pan, tilt, zoom;
    bool empty() const { return !pan && !tilt && !zoom; }
};

std::optional<double> axis_value(HWND field, const wchar_t *name, std::wstring *error) {
    std::wstring text = trimmed(field);
    if (text.empty() || text == L"x" || text == L"X") return std::nullopt;
    double value;
    if (!parse_value(text, value)) {
        if (error && error->empty()) *error = std::wstring(name) + L" must be a number (or blank to keep it)";
        return std::nullopt;
    }
    return value;
}

Target parse_target(std::wstring *error) {
    return Target{axis_value(g->target_pan, L"Pan", error), axis_value(g->target_tilt, L"Tilt", error),
                  axis_value(g->target_zoom, L"Zoom", error)};
}

void update_target_markers() {
    Target t = parse_target(nullptr);
    meter_state(g->meter_pan).target = t.pan;
    meter_state(g->meter_tilt).target = t.tilt;
    meter_state(g->meter_zoom).target = t.zoom;
    for (HWND w : {g->meter_pan, g->meter_tilt, g->meter_zoom}) InvalidateRect(w, nullptr, FALSE);
}

void layout();

void update_controls() {
    bool connected = g->state == State::Connected, idle = g->state == State::Idle;
    for (const auto &row : g->rows) EnableWindow(row.edit, idle);
    for (HWND b : {g->route_local, g->route_ssh, g->vendor_onvif, g->vendor_axis}) EnableWindow(b, idle);
    SetWindowTextW(g->connect, idle ? L"Connect"
                               : g->state == State::Connecting ? L"Cancel"
                               : connected ? L"Disconnect"
                                           : L"Disconnecting\u2026");
    EnableWindow(g->connect, g->state != State::Disconnecting);
    for (HWND w : {g->target_pan, g->target_tilt, g->target_zoom, g->move, g->stop, g->web}) EnableWindow(w, connected);
    EnableWindow(g->start, connected && g->starting && !g->starting->empty());

    const CameraInfo empty_info;
    const CameraInfo &info = g->info ? *g->info : empty_info;
    PlaneState &plane = plane_state(g->plane);
    plane.enabled = connected;
    InvalidateRect(g->plane, nullptr, FALSE);
    EnableWindow(g->refresh_picture, connected && !g->taking_picture);
    struct M { HWND w; AxisRange r; std::optional<Reading> Snapshot::*axis; };
    for (auto [w, range, axis] : {M{g->meter_pan, info.pan, &Snapshot::pan}, M{g->meter_tilt, info.tilt, &Snapshot::tilt},
                                  M{g->meter_zoom, info.zoom, &Snapshot::zoom}}) {
        MeterState &m = meter_state(w);
        m.enabled = connected;
        m.range = range;
        m.value = connected && g->current && (*g->current.*axis) ? std::optional<double>((*g->current.*axis)->value) : std::nullopt;
        m.start = connected && g->starting && (*g->starting.*axis) ? std::optional<double>((*g->starting.*axis)->value) : std::nullopt;
    }
    meter_state(g->meter_zoom).integer = info.zoom_integer;
    update_target_markers();
    InvalidateRect(g->hwnd, nullptr, FALSE);
    for (HWND b : {g->connect, g->move, g->start, g->stop, g->web, g->refresh_picture}) InvalidateRect(b, nullptr, FALSE);
}

void show_message(const std::wstring &text, bool error) {
    g->message = text;
    g->message_error = error;
    InvalidateRect(g->hwnd, &g->message_rc, FALSE);
}

// ---- connection -------------------------------------------------------------------

std::optional<Settings> read_settings(std::wstring &error) {
    auto port = [&](HWND field, const wchar_t *name, int &out) {
        try {
            size_t used = 0;
            std::wstring text = trimmed(field);
            int v = std::stoi(text, &used);
            if (used == text.size() && v >= 1 && v <= 65535) {
                out = v;
                return true;
            }
        } catch (...) {
        }
        error = std::wstring(name) + L" must be an integer between 1 and 65535";
        return false;
    };
    Settings s = g->settings;
    s.route = g->route;
    s.vendor = g->vendor;
    s.camera_host = trimmed(g->cam_host);
    s.camera_user = trimmed(g->cam_user);
    if (s.camera_host.empty()) { error = L"Camera address is required"; return std::nullopt; }
    if (s.camera_user.empty()) { error = L"Camera username is required"; return std::nullopt; }
    if (!port(g->cam_port, L"Camera HTTP port", s.camera_port)) return std::nullopt;
    if (!port(g->https_port, L"HTTPS port", s.https_port)) return std::nullopt;
    try {
        size_t used = 0;
        std::wstring text = trimmed(g->profile);
        s.profile = std::stoi(text, &used);
        if (used != text.size() || s.profile < 0) throw 0;
    } catch (...) {
        error = L"Profile index must be a nonnegative integer";
        return std::nullopt;
    }
    if (!parse_value(trimmed(g->timeout), s.timeout) || s.timeout <= 0 || s.timeout > 300) {
        error = L"Timeout must be a number of seconds between 0 and 300";
        return std::nullopt;
    }
    s.ssh_host = trimmed(g->ssh_host);
    s.ssh_user = trimmed(g->ssh_user);
    s.ssh_identity = trimmed(g->ssh_key);
    if (s.route == 1) {
        if (s.ssh_host.empty()) { error = L"SSH host is required"; return std::nullopt; }
        if (s.ssh_user.empty()) { error = L"SSH username is required"; return std::nullopt; }
        if (!port(g->ssh_port, L"SSH port", s.ssh_port)) return std::nullopt;
    }
    return s;
}

void refresh();
void disconnect(std::function<void()> then = nullptr);
void take_picture();

// ---- saved logins ---------------------------------------------------------------------

enum { kTyped = -1, kManage = -2, kUnlock = -3 };

void fill_logins(HWND box, LoginStore store) {
    int current = static_cast<int>(SendMessageW(box, CB_GETCURSEL, 0, 0));
    std::wstring previous;
    if (current > 0 && static_cast<int>(SendMessageW(box, CB_GETITEMDATA, current, 0)) >= 0) {
        previous.resize(static_cast<size_t>(SendMessageW(box, CB_GETLBTEXTLEN, current, 0)) + 1);
        SendMessageW(box, CB_GETLBTEXT, current, reinterpret_cast<LPARAM>(previous.data()));
        previous.resize(wcslen(previous.c_str()));
    }
    SendMessageW(box, CB_RESETCONTENT, 0, 0);
    auto add = [box](const std::wstring &text, int tag) {
        LRESULT i = SendMessageW(box, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        SendMessageW(box, CB_SETITEMDATA, i, tag);
    };
    add(L"Type a login", kTyped);
    VaultState state = vault_state();
    if (state == VaultState::Unavailable) {
        add(L"Saved logins unavailable", kTyped);
    } else if (state == VaultState::Locked) {
        add(L"Unlock Saved Logins\u2026", kUnlock);
    } else {
        int i = 0;
        for (const SavedLogin &l : vault_logins(store)) add(l.name, i++);
    }
    add(L"Manage Logins\u2026", kManage);
    LRESULT found = previous.empty() ? CB_ERR : SendMessageW(box, CB_FINDSTRINGEXACT, 0, reinterpret_cast<LPARAM>(previous.c_str()));
    SendMessageW(box, CB_SETCURSEL, found > 0 ? found : 0, 0);
}

void reload_logins() {
    fill_logins(g->ssh_login, LoginStore::Ssh);
    fill_logins(g->cam_login, LoginStore::Camera);
}

void login_chosen(HWND box) {
    LoginStore store = box == g->ssh_login ? LoginStore::Ssh : LoginStore::Camera;
    int item = static_cast<int>(SendMessageW(box, CB_GETCURSEL, 0, 0));
    int tag = item < 0 ? kTyped : static_cast<int>(SendMessageW(box, CB_GETITEMDATA, item, 0));
    if (tag == kManage) {
        SendMessageW(box, CB_SETCURSEL, 0, 0);
        show_manage_logins(g->hwnd, store);
        return;
    }
    if (tag == kUnlock) {
        SendMessageW(box, CB_SETCURSEL, 0, 0);
        if (vault_unlock_dialog(g->hwnd)) SendMessageW(box, CB_SHOWDROPDOWN, TRUE, 0);
        return;
    }
    if (tag < 0) return;
    std::vector<SavedLogin> logins = vault_logins(store);
    if (tag >= static_cast<int>(logins.size())) return;
    g->filling = true;
    SetWindowTextW(store == LoginStore::Ssh ? g->ssh_user : g->cam_user, logins[tag].user.c_str());
    SetWindowTextW(store == LoginStore::Ssh ? g->ssh_password : g->cam_password, logins[tag].password.c_str());
    g->filling = false;
    for (auto &l : logins) SecureZeroMemory(l.password.data(), l.password.size() * sizeof(wchar_t));
}

void choose_saved_login(LoginStore store, int index) {  // test hook
    HWND box = store == LoginStore::Ssh ? g->ssh_login : g->cam_login;
    int count = static_cast<int>(SendMessageW(box, CB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i)
        if (static_cast<int>(SendMessageW(box, CB_GETITEMDATA, i, 0)) == index) {
            SendMessageW(box, CB_SETCURSEL, i, 0);
            login_chosen(box);
            return;
        }
}

// The name of the saved login a section's fields still hold, or `fallback`.
std::wstring chosen_login_name(HWND box, const std::wstring &fallback) {
    int item = static_cast<int>(SendMessageW(box, CB_GETCURSEL, 0, 0));
    if (item < 0 || static_cast<int>(SendMessageW(box, CB_GETITEMDATA, item, 0)) < 0) return fallback;
    std::wstring text(static_cast<size_t>(SendMessageW(box, CB_GETLBTEXTLEN, item, 0)) + 1, L'\0');
    SendMessageW(box, CB_GETLBTEXT, item, reinterpret_cast<LPARAM>(text.data()));
    text.resize(wcslen(text.c_str()));
    return text;
}

void save_ticked_logins(std::vector<std::pair<SavedLogin, LoginStore>> &saved) {
    g->save_ssh_on = g->save_cam_on = false;
    InvalidateRect(g->save_ssh, nullptr, FALSE);
    InvalidateRect(g->save_cam, nullptr, FALSE);
    if (!saved.empty()) {
        if (!vault_ensure_writable(g->hwnd)) {
            log_line(L"Logins not saved.");
        } else {
            for (auto &[login, store] : saved) {
                std::wstring error, kind = store == LoginStore::Ssh ? L"SSH" : L"camera";
                // PTZ_SNAPSHOT_LOGIN_NAME is the test hook's answer: the name a user would type.
                const wchar_t *test_name = _wgetenv(L"PTZ_SNAPSHOT_LOGIN_NAME");
                std::wstring name = test_name ? test_name : vault_ask_name(g->hwnd, store, login.user, login.name);
                if (name.empty()) continue;
                login.name = name;
                if (vault_save(login, store, -1, &error)) log_line(L"Saved " + kind + L" login " + login.name + L".");
                else log_line(L"Couldn't save the " + kind + L" login: " + error, true);
            }
        }
    }
    for (auto &[login, store] : saved) SecureZeroMemory(login.password.data(), login.password.size() * sizeof(wchar_t));
}

void connect() {
    // Capture secrets first: nothing below may clear the fields before this.
    std::wstring camera_password = text_of(g->cam_password), ssh_password = text_of(g->ssh_password);
    std::wstring error;
    auto settings = read_settings(error);
    if (!settings) {
        show_message(error, true);
        return;
    }
    if (camera_password.empty()) {
        show_message(L"Camera password is required", true);
        SetFocus(g->cam_password);
        return;
    }
    g->settings = *settings;
    g->settings.save();
    g->state = State::Connecting;
    const Settings &s = g->settings;
    show_message(s.route ? L"Opening SSH tunnel\u2026" : L"Connecting\u2026", false);
    log_line(L"Connecting: " + (s.route ? L"SSH via " + s.ssh_user + L"@" + s.ssh_host : std::wstring(L"local")) + L" \u2192 " +
             s.camera_host + L":" + std::to_wstring(s.camera_port) + L" \u00b7 " + (s.vendor ? L"Axis VAPIX" : L"ONVIF"));
    update_controls();
    // What Save login keeps once the connection has proved it.
    std::vector<std::pair<SavedLogin, LoginStore>> saved;
    if (g->save_cam_on)
        saved.push_back({{chosen_login_name(g->cam_login, s.camera_user), s.camera_user, camera_password}, LoginStore::Camera});
    if (s.route == 1 && g->save_ssh_on)
        saved.push_back({{chosen_login_name(g->ssh_login, s.ssh_user), s.ssh_user, ssh_password}, LoginStore::Ssh});
    link_connect(g->link, s, camera_password, s.route ? ssh_password : L"", [saved](LinkResult result, CameraInfo info) mutable {
        if (result.ok) {
            g->info = info;
            g->state = State::Connected;
            g->starting.reset();
            g->current.reset();
            g->failures = 0;
            g->paused = false;
            {
                PlaneState &plane = plane_state(g->plane);
                plane.view_moves = info.view_moves;
                plane.picture.reset();
                plane.picture_note.clear();
                plane.picture_target.reset();
            }
            g->picture_due = 0;
            take_picture();
            show_message(L"Connected. No movement has been sent.", false);
            log_line(L"Connected: " + info.name);
            log_line(info.units);
            for (auto [name, range] : {std::pair{L"Pan", info.pan}, std::pair{L"Tilt", info.tilt}, std::pair{L"Zoom", info.zoom}})
                log_line(std::wstring(L"  ") + name + L" range: " +
                         (range.available ? format_value(range.min) + L" \u2026 " + format_value(range.max) : L"not advertised"));
            refresh();
            save_ticked_logins(saved);
        } else {
            g->state = State::Idle;
            show_message(result.error, true);
            log_line(L"Connection failed: " + result.error, true);
        }
        update_controls();
    });
    SecureZeroMemory(camera_password.data(), camera_password.size() * sizeof(wchar_t));
    SecureZeroMemory(ssh_password.data(), ssh_password.size() * sizeof(wchar_t));
}

void disconnect(std::function<void()> then) {
    if (g->state != State::Connected && g->state != State::Connecting) {
        if (then) then();
        return;
    }
    if (g->state == State::Connecting) link_cancel_connect(g->link);
    g->state = State::Disconnecting;
    update_controls();
    log_line(L"Disconnecting; waiting for any in-flight request (no PTZ stop is sent)");
    link_disconnect(g->link, [then] {
        g->state = State::Idle;
        g->info.reset();
        g->current.reset();
        g->starting.reset();
        plane_state(g->plane).picture.reset();
        plane_state(g->plane).picture_target.reset();
        g->picture_due = 0;
        show_message(L"Disconnected.", false);
        log_line(L"Disconnected");
        update_controls();
        if (then) then();
    });
}

void toggle_connection() {
    switch (g->state) {
        case State::Idle: connect(); break;
        case State::Connecting:
            link_cancel_connect(g->link);
            log_line(L"Cancelling connection\u2026");
            break;
        case State::Connected: disconnect(); break;
        case State::Disconnecting: break;
    }
}

void link_lost(const std::wstring &message) {
    log_line(message, true);
    show_message(message, true);
    disconnect();
}

// ---- position, moves --------------------------------------------------------------

void refresh() {
    link_position(g->link, [](LinkResult result, Snapshot snapshot) {
        if (g->state != State::Connected) return;
        g->next_refresh = GetTickCount64() + 2000;
        if (result.ok) {
            g->failures = 0;
            g->last_read = GetTickCount64();
            if (!g->starting) {
                g->starting = snapshot;
                log_line(L"Starting coordinates: " + describe(snapshot));
            }
            if (g->current && !(snapshot == *g->current)) log_line(L"Current coordinates: " + describe(snapshot));
            g->current = snapshot;
        } else {
            if (result.link_lost) {
                link_lost(result.error);
                return;
            }
            ++g->failures;
            log_line(L"Read failed: " + result.error, true);
            if (!g->starting) g->starting = Snapshot{};
            if (g->failures >= 3) {
                g->paused = true;
                log_line(L"Auto-refresh paused after 3 failures. Press Ctrl+R to retry.", true);
            }
        }
        update_controls();
    });
    update_controls();
}

void tick() {
    if (g->state != State::Connected || link_busy(g->link)) return;
    ULONGLONG now = GetTickCount64();
    if (g->picture_due) {
        // A fresh picture once the camera has stopped: a reading taken after the move that says it isn't moving,
        // or, when the camera doesn't say, three seconds.
        ULONGLONG age = now - g->picture_due;
        bool reported = g->current && !g->current->move_status.empty();
        bool stopped = g->last_read > g->picture_due + 800 && reported && g->current->move_status != L"MOVING";
        if (stopped || (age >= 3000 && (!reported || g->paused)) || age >= 30000) {
            g->picture_due = 0;
            take_picture();
            return;
        }
    }
    if (!g->paused && now >= g->next_refresh) refresh();
}

void refresh_now() {
    if (g->state != State::Connected) return;
    if (g->paused || g->failures) log_line(L"Auto-refresh resumed");
    g->failures = 0;
    g->paused = false;
    g->next_refresh = 0;
    if (!link_busy(g->link)) refresh();
    update_controls();
}

void toggle_pause() {
    if (g->state != State::Connected) return;
    g->paused = !g->paused;
    log_line(g->paused ? L"Auto-refresh paused" : L"Auto-refresh resumed");
    update_controls();
}

// ---- picture (spec I10-I11) ---------------------------------------------------------

// The plane takes the picture's shape: the window grows or shrinks in height, keeping its top edge, within the screen.
void fit_window_to_picture() {
    if (!IsWindowVisible(g->hwnd) || IsZoomed(g->hwnd) || IsIconic(g->hwnd)) return;
    const PlaneState &s = plane_state(g->plane);
    double pw = s.picture ? s.picture_width : 16, ph = s.picture ? s.picture_height : 9;
    RECT pc;
    GetClientRect(g->plane, &pc);
    if (pw <= 0 || pc.right <= 0) return;
    int wanted = static_cast<int>(std::lround((pc.right - 2) * ph / pw + 2));
    int delta = wanted - pc.bottom;
    if (std::abs(delta) < 2) return;
    RECT wr;
    GetWindowRect(g->hwnd, &wr);
    int height = wr.bottom - wr.top + delta, top = wr.top;
    MONITORINFO mi{sizeof mi};
    if (GetMonitorInfoW(MonitorFromWindow(g->hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
        height = std::min<int>(height, mi.rcWork.bottom - mi.rcWork.top);
        if (top + height > mi.rcWork.bottom) top = mi.rcWork.bottom - height;
        top = std::max<int>(top, mi.rcWork.top);
    }
    SetWindowPos(g->hwnd, nullptr, wr.left, top, wr.right - wr.left, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

std::wstring clock_now() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t text[16];
    swprintf(text, 16, L"%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
    return text;
}

void take_picture() {
    if (g->state != State::Connected || g->taking_picture) return;
    g->taking_picture = true;
    link_picture(g->link, [](LinkResult result, std::string bytes) {
        g->taking_picture = false;
        if (g->state != State::Connected) return;
        PlaneState &s = plane_state(g->plane);
        if (result.ok) {
            int old_w = s.picture ? s.picture_width : 0, old_h = s.picture ? s.picture_height : 0;
            bool stale = s.picture_stale;
            if (!plane_set_picture(g->plane, bytes)) {
                result.ok = false;
                result.error = L"The camera's picture could not be read";
            } else {
                if (stale) s.picture_target.reset();  // the camera has moved since
                s.picture_taken = clock_now();
                s.picture_stale = false;
                s.picture_note.clear();
                if (s.picture_width != old_w || s.picture_height != old_h) {
                    log_line(L"Camera picture: " + std::to_wstring(s.picture_width) + L" \u00d7 " + std::to_wstring(s.picture_height) +
                             (g->info && g->info->view_moves ? L"" : L"; this camera can't aim from its picture"));
                    fit_window_to_picture();
                }
            }
        } else if (result.link_lost) {
            link_lost(result.error);
            return;
        }
        if (!result.ok) {
            if (s.picture_note != result.error) log_line(L"Picture failed: " + result.error, true);
            s.picture_note = result.error;
            s.picture_stale = false;
        }
        InvalidateRect(g->plane, nullptr, FALSE);
        update_controls();
    });
    update_controls();
}

// The picture is out of date once the camera moves; a new one is taken when it stops.
void camera_moved() {
    PlaneState &s = plane_state(g->plane);
    if (!s.picture) return;
    s.picture_stale = true;
    g->picture_due = GetTickCount64();
    InvalidateRect(g->plane, nullptr, FALSE);
}

void aim_in_picture(float x, float y) {
    plane_state(g->plane).picture_target = std::make_pair(std::clamp(x, -1.0f, 1.0f), std::clamp(y, -1.0f, 1.0f));
    InvalidateRect(g->plane, nullptr, FALSE);
}

void send_picture_move(std::pair<float, float> point) {
    if (!g->info || !g->info->view_moves) {
        log_line(L"This camera can't aim from its picture; use the target fields or the travel meters.", true);
        return;
    }
    wchar_t text[96];
    swprintf(text, 96, L"%.2f %s, %.2f %s of the picture", std::fabs(point.first), point.first < 0 ? L"left" : L"right",
             std::fabs(point.second), point.second < 0 ? L"down" : L"up");
    std::wstring describe = text;
    link_move_in_view(g->link, point.first, point.second, [describe](LinkResult result) {
        if (result.ok) {
            log_line(L"Move toward the picture point accepted: " + describe + L" (movement may still be in progress)");
            g->next_refresh = 0;
            camera_moved();
        } else if (result.link_lost) {
            link_lost(result.error);
            return;
        } else {
            log_line(L"Move toward the picture point failed: " + result.error, true);
        }
        update_controls();
    });
    update_controls();
}

void send(const Target &t, const std::wstring &what) {
    if (link_busy(g->link)) log_line(L"Request in progress; move queued behind it");
    std::wstring summary;
    for (auto [name, v] : {std::pair{L"pan", t.pan}, std::pair{L"tilt", t.tilt}, std::pair{L"zoom", t.zoom}})
        if (v) summary += (summary.empty() ? L"" : L", ") + std::wstring(name) + L"=" + format_value(*v);
    link_move(g->link, t.pan, t.tilt, t.zoom, [what, summary](LinkResult result) {
        if (result.ok) {
            log_line(what + L" accepted: " + summary + L" (movement may still be in progress)");
            g->next_refresh = 0;
            camera_moved();
        } else if (result.link_lost) {
            link_lost(result.error);
            return;
        } else {
            log_line(what + L" failed: " + result.error, true);
        }
        update_controls();
    });
    update_controls();
}

void send_move() {
    if (g->state != State::Connected) return;
    if (auto point = plane_state(g->plane).picture_target) {
        send_picture_move(*point);
        return;
    }
    std::wstring error;
    Target t = parse_target(&error);
    if (!error.empty()) {
        log_line(error, true);
        MessageBeep(MB_ICONWARNING);
        return;
    }
    if (t.empty()) {
        log_line(L"All axes unchanged; no move sent.");
        return;
    }
    send(t, L"Move");
}

void return_to_start() {
    if (g->state != State::Connected) return;
    if (!g->starting || g->starting->empty()) {
        log_line(L"Starting coordinates unavailable; no return move sent", true);
        return;
    }
    const Snapshot &s = *g->starting;
    std::wstring missing;
    for (auto [name, r] : {std::pair{L"pan", &s.pan}, std::pair{L"tilt", &s.tilt}, std::pair{L"zoom", &s.zoom}})
        if (!*r) missing += (missing.empty() ? L"" : L", ") + std::wstring(name);
    log_line(L"Returning to starting coordinates" + (missing.empty() ? L"" : L"; unavailable axes left unchanged: " + missing));
    Target t;
    if (s.pan) t.pan = s.pan->value;
    if (s.tilt) t.tilt = s.tilt->value;
    if (s.zoom) t.zoom = s.zoom->value;
    // Show the start position as the target, so the plane and meter handles move there too.
    SetWindowTextW(g->target_pan, s.pan ? s.pan->text.c_str() : L"");
    SetWindowTextW(g->target_tilt, s.tilt ? s.tilt->text.c_str() : L"");
    SetWindowTextW(g->target_zoom, s.zoom ? s.zoom->text.c_str() : L"");
    update_target_markers();
    send(t, L"Return");
}

void stop_motion() {
    if (g->state != State::Connected) return;
    link_stop(g->link, [](LinkResult result) {
        if (result.ok) {
            log_line(L"Stop accepted");
            camera_moved();
        } else if (result.link_lost) { link_lost(result.error); return; }
        else log_line(L"Stop failed: " + result.error, true);
        g->next_refresh = 0;
    });
}

void open_web() {
    if (g->state != State::Connected || g->web_pending) return;
    g->web_pending = true;
    log_line(L"Locating camera web interface (HTTP first, HTTPS fallback; no credentials sent)\u2026");
    link_web_url(g->link, [](LinkResult result, std::wstring url, std::wstring notice) {
        g->web_pending = false;
        if (result.ok) {
            log_line(L"Camera web interface: " + url + (notice.empty() ? L"" : L" \u2014 " + notice));
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(g->hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                MessageBoxW(g->hwnd, (L"The browser could not be opened automatically. Open this address:\n" + url).c_str(),
                            L"Open the camera web interface", MB_OK | MB_ICONINFORMATION);
        } else if (result.link_lost) {
            link_lost(result.error);
        } else {
            log_line(L"Camera web interface: " + result.error, true);
        }
    });
}

void show_help() {
    MessageBoxW(g->hwnd,
                L"Targets are absolute positions, not increments. Units depend on the vendor:\n"
                L"\u2022 ONVIF: the camera's advertised absolute space (commonly pan/tilt \u22121\u20261, zoom 0\u20261; not degrees).\n"
                L"\u2022 Axis: pan/tilt in degrees; zoom is an integer in the native range.\n\n"
                L"Leave a target field blank (or type x) to keep that axis unchanged.\n"
                L"The camera picture shows what the camera sees: click a point to aim at it, double-click (or Move) to turn "
                L"the camera so that point comes to the centre. A new picture comes once the camera stops, or with Refresh "
                L"Picture (Ctrl+Shift+R).\n"
                L"Drag along a travel meter to aim that axis alone, double-click it to aim and move.\n\n"
                L"Starting coordinates are the first reading after connecting and stay fixed. "
                L"Return to Start (Ctrl+B) sends an absolute move there.\n"
                L"Current coordinates refresh every 2 seconds (read-only) unless paused (Ctrl+P).\n"
                L"Stop (Ctrl+.) sends a PTZ stop. Disconnecting never sends a stop.\n"
                L"Web Interface (Ctrl+L) opens the camera's own page in your browser: HTTP first, HTTPS fallback. "
                L"No credentials are sent.\n\n"
                L"Shortcuts: Ctrl+K connect, Ctrl+Shift+K disconnect, Ctrl+Enter move, Ctrl+R refresh, Ctrl+Shift+L saved logins, "
                L"Ctrl+Alt+1\u20265 phosphor.",
                L"Coordinate help", MB_OK | MB_ICONINFORMATION);
}

// ---- theme, fonts, layout ---------------------------------------------------------

void apply_theme() {
    g->theme = make_theme(g->phosphor);
    instruments_set_theme(g->theme, g->dpi);
    BOOL dark = g->theme.dark;
    DwmSetWindowAttribute(g->hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    if (g->field_brush) DeleteObject(g->field_brush);
    g->field_brush = CreateSolidBrush(g->theme.field);
    SendMessageW(g->log, EM_SETBKGNDCOLOR, 0, g->theme.panel);
    SetWindowTheme(g->log, g->theme.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    for (HWND box : {g->ssh_login, g->cam_login}) SetWindowTheme(box, g->theme.dark ? L"DarkMode_CFD" : L"CFD", nullptr);
    // Recolour existing log text (warnings lose their colour; new ones keep it).
    CHARFORMAT2W format{};
    format.cbSize = sizeof format;
    format.dwMask = CFM_COLOR;
    format.crTextColor = g->theme.text;
    SendMessageW(g->log, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
    RedrawWindow(g->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
}

void make_fonts() {
    for (HFONT f : {g->field_font, g->log_font, g->button_font})
        if (f) DeleteObject(f);
    auto font = [](int px, int weight, const wchar_t *face) {
        return CreateFontW(-S(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    };
    g->field_font = font(13, FW_NORMAL, mono_family());
    g->log_font = font(12, FW_NORMAL, mono_family());
    g->button_font = font(13, FW_NORMAL, L"Segoe UI");
    for (const auto &row : g->rows) SendMessageW(row.edit, WM_SETFONT, reinterpret_cast<WPARAM>(g->field_font), TRUE);
    for (HWND w : g->target_fields) SendMessageW(w, WM_SETFONT, reinterpret_cast<WPARAM>(g->field_font), TRUE);
    SendMessageW(g->log, WM_SETFONT, reinterpret_cast<WPARAM>(g->log_font), TRUE);
}

void place_field(HWND edit, const RECT &frame) {
    int h = S(18);
    SetWindowPos(edit, nullptr, frame.left + S(6), frame.top + (frame.bottom - frame.top - h) / 2,
                 frame.right - frame.left - S(12), h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void move(HWND w, int x, int y, int cx, int cy) { SetWindowPos(w, nullptr, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE); }

void layout() {
    RECT rc;
    GetClientRect(g->hwnd, &rc);
    int W_ = rc.right, H = rc.bottom, gap = S(12);
    g->captions.clear();
    g->footer = {gap, H - S(24), W_ - gap, H - S(6)};
    g->sidebar = {gap, gap, gap + S(300), g->footer.top - S(6)};

    // Sidebar.
    int x = g->sidebar.left + S(12), w = g->sidebar.right - g->sidebar.left - S(24), y = g->sidebar.top + S(32);
    auto caption = [&](const wchar_t *text) {
        g->captions.push_back({x, y, text});
        y += S(17);
    };
    caption(L"ROUTE");
    move(g->route_local, x, y, w / 2, S(26));
    move(g->route_ssh, x + w / 2, y, w - w / 2, S(26));
    y += S(34);
    caption(L"CAMERA VENDOR");
    move(g->vendor_onvif, x, y, w / 2, S(26));
    move(g->vendor_axis, x + w / 2, y, w - w / 2, S(26));
    y += S(30);
    g->vendor_note = {x, y, x + w, y + S(30)};
    y += S(36);
    bool ssh = g->route == 1;
    bool first_camera = true, first_advanced = true;
    for (auto &row : g->rows) {
        if (row.ssh && !ssh) {
            ShowWindow(row.edit, SW_HIDE);
            row.frame = {};
            continue;
        }
        if (row.ssh && row.edit == g->ssh_login) caption(L"SSH HOST");
        if (row.edit == g->cam_login && first_camera) {
            if (ssh) y += S(4);
            caption(L"CAMERA");
            first_camera = false;
        }
        if (row.edit == g->https_port && first_advanced) {
            y += S(4);
            caption(L"ADVANCED");
            first_advanced = false;
        }
        row.frame = {x + S(88), y, x + w, y + S(24)};
        if (row.kind == kEdit) place_field(row.edit, row.frame);
        else if (row.kind == kCombo) move(row.edit, row.frame.left, row.frame.top, row.frame.right - row.frame.left, S(240));
        else move(row.edit, row.frame.left, row.frame.top, row.frame.right - row.frame.left, row.frame.bottom - row.frame.top);
        ShowWindow(row.edit, SW_SHOW);
        y += row.kind == kCheck ? S(26) : S(29);
    }
    y += S(8);
    move(g->connect, x, y, w, S(32));
    y += S(40);
    g->message_rc = {x, y, x + w, std::min<LONG>(y + S(90), g->sidebar.bottom - S(8))};

    // Right side.
    int left = g->sidebar.right + gap, right = W_ - gap;
    g->header = {left + S(4), gap, right, gap + S(48)};
    move(g->menu, right - S(34), gap + S(6), S(34), S(28));
    g->chip = {right - S(34) - S(10) - S(150), gap + S(8), right - S(34) - S(10), gap + S(32)};

    int readout_w = S(230);
    int top = gap + S(56);
    int log_h = S(150), target_h = S(78), meters_h = S(150);
    g->log_panel = {left, g->footer.top - S(6) - log_h, right, g->footer.top - S(6)};
    g->target_panel = {left, g->log_panel.top - gap - target_h, right, g->log_panel.top - gap};
    g->meters_panel = {left, g->target_panel.top - gap - meters_h, right - readout_w - gap, g->target_panel.top - gap};
    g->plane_panel = {left, top, right - readout_w - gap, g->meters_panel.top - gap};
    g->readout_panel = {right - readout_w, top, right, g->meters_panel.bottom};

    auto inner = [&](const RECT &p) { return RECT{p.left + S(12), p.top + S(28), p.right - S(12), p.bottom - S(10)}; };
    RECT pi = inner(g->plane_panel);
    move(g->plane, pi.left, pi.top, pi.right - pi.left, pi.bottom - pi.top);
    move(g->refresh_picture, g->plane_panel.right - S(8) - S(24), g->plane_panel.top + S(4), S(24), S(20));
    RECT mi = inner(g->meters_panel);
    int mh = (mi.bottom - mi.top) / 3;
    move(g->meter_pan, mi.left, mi.top, mi.right - mi.left, mh);
    move(g->meter_tilt, mi.left, mi.top + mh, mi.right - mi.left, mh);
    move(g->meter_zoom, mi.left, mi.top + 2 * mh, mi.right - mi.left, mh);

    RECT ti = inner(g->target_panel);
    g->target_frames.clear();
    int fx = ti.left;
    for (HWND f : g->target_fields) {
        RECT frame = {fx, ti.top, fx + S(130), ti.top + S(26)};
        g->target_frames.push_back(frame);
        place_field(f, frame);
        fx += S(138);
    }
    move(g->move, fx, ti.top, S(70), S(26));
    int bx = ti.right;
    for (auto [b, bw] : {std::pair{g->web, 116}, std::pair{g->stop, 60}, std::pair{g->start, 124}}) {
        bx -= S(bw);
        move(b, bx, ti.top, S(bw), S(26));
        bx -= S(8);
    }
    g->target_note = {ti.left, ti.top + S(32), ti.right, ti.top + S(48)};

    RECT li = inner(g->log_panel);
    move(g->log, li.left, li.top, li.right - li.left, li.bottom - li.top);
    InvalidateRect(g->hwnd, nullptr, FALSE);
}

// ---- painting -----------------------------------------------------------------------

RectF RF(const RECT &r) {
    return RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right - r.left),
                 static_cast<float>(r.bottom - r.top));
}

void rounded(Graphics &gr, const RECT &r, float radius, COLORREF fill, COLORREF stroke) {
    GraphicsPath path;
    RectF f = RF(r);
    float d = radius * 2;
    path.AddArc(f.X, f.Y, d, d, 180, 90);
    path.AddArc(f.GetRight() - d, f.Y, d, d, 270, 90);
    path.AddArc(f.GetRight() - d, f.GetBottom() - d, d, d, 0, 90);
    path.AddArc(f.X, f.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
    SolidBrush brush(C(fill));
    gr.FillPath(&brush, &path);
    Pen pen(C(stroke), 1);
    gr.DrawPath(&pen, &path);
}

void text(Graphics &gr, const std::wstring &s, RectF box, float px, COLORREF color, bool mono = false,
          INT style = FontStyleRegular, StringAlignment align = StringAlignmentNear, bool wrap = false) {
    Font font(mono ? mono_family() : L"Segoe UI", SF(px), style, UnitPixel);
    SolidBrush brush(C(color));
    StringFormat format;
    format.SetAlignment(align);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    if (!wrap) format.SetFormatFlags(StringFormatFlagsNoWrap);
    gr.DrawString(s.c_str(), -1, &font, box, &format, &brush);
}

void panel(Graphics &gr, const RECT &r, const std::wstring &caption) {
    rounded(gr, r, SF(6), g->theme.panel, g->theme.border);
    text(gr, caption, RectF(r.left + SF(12), r.top + SF(8), r.right - r.left - SF(24), SF(16)), 10, g->theme.secondary,
         true, FontStyleBold);
}

void paint(HDC dc, const RECT &rc) {
    Graphics gr(dc);
    gr.SetSmoothingMode(SmoothingModeAntiAlias);
    gr.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    gr.Clear(C(g->theme.background));
    const Theme &t = g->theme;
    bool connected = g->state == State::Connected;

    panel(gr, g->sidebar, L"SECURE LINK CONFIGURATION");
    for (const auto &c : g->captions)
        text(gr, c.text, RectF(static_cast<float>(c.x), static_cast<float>(c.y), SF(260), SF(15)), 10, t.secondary, true,
             FontStyleBold);
    text(gr, g->vendor == 1 ? L"Native VAPIX with HTTP Digest. Pan/tilt in degrees, integer zoom."
                            : L"ONVIF absolute coordinates with camera ONVIF credentials.",
         RF(g->vendor_note), 11.5f, t.secondary, false, FontStyleRegular, StringAlignmentNear, true);
    for (const auto &row : g->rows) {
        if (row.frame.right == 0) continue;
        text(gr, row.label, RectF(row.frame.left - SF(92), row.frame.top + SF(3), SF(84), SF(18)), 12.5f, t.text, false,
             FontStyleRegular, StringAlignmentFar);
        if (row.kind == kEdit) rounded(gr, row.frame, SF(3), t.field, t.border);
    }
    text(gr, g->message, RF(g->message_rc), 11.5f, g->message_error ? t.warning : t.secondary, false, FontStyleRegular,
         StringAlignmentNear, true);

    // Header.
    const Settings &s = g->settings;
    std::wstring title = connected && g->info ? s.camera_host + L" \u00b7 " + g->info->name
                       : g->state == State::Connecting ? L"Connecting to " + s.camera_host + L"\u2026"
                                                       : L"No camera link";
    std::wstring units = connected && g->info ? g->info->units
                                              : L"Choose a route and vendor, enter the camera details, then Connect.";
    float header_w = static_cast<float>(g->chip.left - g->header.left - S(12));
    text(gr, title, RectF(static_cast<float>(g->header.left), static_cast<float>(g->header.top), header_w, SF(24)), 17, t.text,
         false, FontStyleBold);
    text(gr, units, RectF(static_cast<float>(g->header.left), g->header.top + SF(26), header_w, SF(18)), 12, t.secondary);

    std::wstring chip;
    COLORREF chip_color = t.secondary;
    switch (g->state) {
        case State::Idle: chip = L"OFFLINE"; chip_color = t.secondary; break;
        case State::Connecting: chip = L"LINKING"; chip_color = t.dark && g->phosphor != Phosphor::System ? t.accent : t.start; break;
        case State::Disconnecting: chip = L"CLOSING"; chip_color = t.start; break;
        case State::Connected: {
            bool ok = g->current && !g->current->empty();
            chip = link_busy(g->link) ? L"REQUEST" : g->paused ? L"READS PAUSED" : ok ? L"POSITION OK" : L"NO POSITION";
            chip_color = g->paused || !ok ? t.start : t.good;
            break;
        }
    }
    {
        Font font(mono_family(), SF(11), FontStyleBold, UnitPixel);
        RectF box;
        std::wstring label = L"\u25cf " + chip;
        gr.MeasureString(label.c_str(), -1, &font, PointF(0, 0), &box);
        RECT pill = {g->chip.right - static_cast<LONG>(box.Width) - S(20), g->chip.top, g->chip.right, g->chip.bottom};
        GraphicsPath path;
        RectF f = RF(pill);
        float d = f.Height;
        path.AddArc(f.X, f.Y, d, d, 90, 180);
        path.AddArc(f.GetRight() - d, f.Y, d, d, 270, 180);
        path.CloseFigure();
        Pen pen(C(chip_color, 140), 1);
        gr.DrawPath(&pen, &path);
        text(gr, label, RectF(f.X, f.Y + SF(4), f.Width, f.Height), 11, chip_color, true, FontStyleBold, StringAlignmentCenter);
    }

    panel(gr, g->plane_panel, L"CAMERA PICTURE // VIEW \u2014 CLICK TO AIM, DOUBLE-CLICK TO MOVE THERE");
    panel(gr, g->meters_panel, L"AXIS TRAVEL // % OF ADVERTISED LIMITS \u2014 DRAG TO AIM, DOUBLE-CLICK TO MOVE");
    panel(gr, g->readout_panel, L"CURRENT COORDINATES");
    panel(gr, g->target_panel, L"TARGET // ABSOLUTE MOVE");
    panel(gr, g->log_panel, L"EVENT BUFFER // SESSION");

    // Readouts: camera's own digits, or an em dash when unavailable.
    const CameraInfo empty_info;
    const CameraInfo &info = g->info ? *g->info : empty_info;
    auto range_text = [](const AxisRange &r) {
        return r.available ? format_value(r.min) + L" \u2026 " + format_value(r.max) : std::wstring(L"range not advertised");
    };
    float rx = g->readout_panel.left + SF(12), ry = g->readout_panel.top + SF(30), rw = g->readout_panel.right - rx - SF(12);
    struct Item { const wchar_t *label; std::optional<std::wstring> value; std::wstring detail; };
    auto reading = [&](const std::optional<Reading> Snapshot::*axis) -> std::optional<std::wstring> {
        if (!connected || !g->current || !((*g->current).*axis)) return std::nullopt;
        return ((*g->current).*axis)->text;
    };
    std::optional<std::wstring> motion;
    if (connected && g->current && !g->current->move_status.empty()) motion = g->current->move_status;
    Item items[] = {
        {L"PAN", reading(&Snapshot::pan), connected ? range_text(info.pan) : L""},
        {L"TILT", reading(&Snapshot::tilt), connected ? range_text(info.tilt) : L""},
        {L"ZOOM", reading(&Snapshot::zoom),
         connected ? range_text(info.zoom) + (info.zoom_integer ? L" \u00b7 integer" : L"") : L""},
        {L"MOTION", motion, connected && g->current && !motion ? L"not reported by camera" : L""},
    };
    for (const auto &item : items) {
        text(gr, item.label, RectF(rx, ry, rw, SF(14)), 10, t.secondary, true, FontStyleBold);
        text(gr, item.value.value_or(L"\u2014"), RectF(rx, ry + SF(14), rw, SF(30)), 22,
             item.value ? t.primary : t.secondary, true, FontStyleBold);
        text(gr, item.detail, RectF(rx, ry + SF(44), rw, SF(14)), 10, t.secondary, true);
        ry += SF(66);
    }
    text(gr, L"Starting: " + (connected && g->starting ? describe(*g->starting) : std::wstring(L"\u2014")),
         RectF(rx, ry, rw, std::max(SF(14), g->readout_panel.bottom - ry - SF(10))), 11, t.start, true, FontStyleRegular,
         StringAlignmentNear, true);

    for (size_t i = 0; i < g->target_frames.size(); ++i) {
        bool enabled = IsWindowEnabled(g->target_fields[i]);
        rounded(gr, g->target_frames[i], SF(3), t.field, enabled ? t.border : blend(t.panel, t.border, 0.5));
    }
    text(gr, L"Absolute targets, not increments. Leave a field blank (or x) to keep that axis.", RF(g->target_note), 11.5f,
         t.secondary);

    std::wstring route = s.route ? L"SSH " + s.ssh_user + L"@" + s.ssh_host + L":" + std::to_wstring(s.ssh_port) : L"LOCAL";
    std::wstring foot = connected ? route + L"  \u00b7  " + s.camera_host + L":" + std::to_wstring(s.camera_port) + L"  \u00b7  " +
                                        (g->paused ? L"auto-refresh PAUSED (Ctrl+R resumes)" : L"2 s read-only refresh") +
                                        L"  \u00b7  " + (link_busy(g->link) ? L"busy" : L"ready")
                                  : route + L"  \u00b7  credentials stay in memory only  \u00b7  Enter connects";
    text(gr, foot, RF(g->footer), 10, t.secondary, true);
    (void)rc;
}

void draw_button(const DRAWITEMSTRUCT &d) {
    const Theme &t = g->theme;
    int id = static_cast<int>(d.CtlID);
    bool disabled = d.itemState & ODS_DISABLED, pressed = d.itemState & ODS_SELECTED, focus = d.itemState & ODS_FOCUS;
    bool segment = id >= ID_ROUTE_LOCAL && id <= ID_VENDOR_AXIS;
    bool selected = (id == ID_ROUTE_LOCAL && g->route == 0) || (id == ID_ROUTE_SSH && g->route == 1) ||
                    (id == ID_VENDOR_ONVIF && g->vendor == 0) || (id == ID_VENDOR_AXIS && g->vendor == 1);
    bool primary = id == ID_CONNECT || id == ID_MOVE;
    bool check = id == ID_SAVE_SSH || id == ID_SAVE_CAM;

    RECT r = d.rcItem;
    if (id == ID_REFRESH_PICTURE) {  // a circular arrow, framed while pressed
        HDC mem = CreateCompatibleDC(d.hDC);
        HBITMAP bitmap = CreateCompatibleBitmap(d.hDC, r.right, r.bottom);
        HGDIOBJ old = SelectObject(mem, bitmap);
        {
            Graphics gr(mem);
            gr.SetSmoothingMode(SmoothingModeAntiAlias);
            gr.Clear(C(t.panel));
            if (pressed) {
                RECT inset = {0, 0, r.right - 1, r.bottom - 1};
                rounded(gr, inset, SF(4), blend(t.panel, t.text, 0.12), t.border);
            }
            COLORREF ink = disabled ? blend(t.panel, t.text, 0.4) : t.text;
            float cx = r.right / 2.0f, cy = r.bottom / 2.0f, radius = SF(5.5f);
            Pen pen(C(ink), SF(1.6f));
            pen.SetStartCap(LineCapRound);
            pen.SetEndCap(LineCapRound);
            // GDI+ angles run clockwise from 3 o'clock: from the arrow's end at -60 degrees, back round to 0 degrees.
            gr.DrawArc(&pen, cx - radius, cy - radius, 2 * radius, 2 * radius, -60, -300);
            PointF tip(cx + radius * 0.5f, cy - radius * 0.8660254f), along(0.8660254f, 0.5f), across(-0.5f, 0.8660254f);
            auto at = [&](float a, float b) { return PointF(tip.X + along.X * a + across.X * b, tip.Y + along.Y * a + across.Y * b); };
            PointF head[] = {at(SF(3.2f), 0), at(-SF(1.2f), SF(3)), at(-SF(1.2f), -SF(3))};
            SolidBrush brush(C(ink));
            gr.FillPolygon(&brush, head, 3);
        }
        BitBlt(d.hDC, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bitmap);
        DeleteDC(mem);
        return;
    }
    if (check) {
        bool on = id == ID_SAVE_SSH ? g->save_ssh_on : g->save_cam_on;
        HDC mem = CreateCompatibleDC(d.hDC);
        HBITMAP bitmap = CreateCompatibleBitmap(d.hDC, r.right, r.bottom);
        HGDIOBJ old = SelectObject(mem, bitmap);
        {
            Graphics gr(mem);
            gr.SetSmoothingMode(SmoothingModeAntiAlias);
            gr.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
            gr.Clear(C(t.panel));
            int box = S(14), top = (r.bottom - box) / 2;
            RECT b = {0, top, box, top + box};
            COLORREF ink = disabled ? blend(t.panel, t.text, 0.45) : t.text;
            rounded(gr, b, SF(3), on ? (disabled ? blend(t.panel, t.accent, 0.5) : t.accent) : t.field,
                    on ? t.accent : (disabled ? blend(t.panel, t.border, 0.6) : t.primary));
            if (on) {
                Pen pen(C(g->phosphor == Phosphor::System ? RGB(255, 255, 255) : t.background), SF(1.8f));
                PointF tick[] = {PointF(SF(3.5f), top + SF(7.5f)), PointF(SF(6), top + SF(10.5f)),
                                 PointF(SF(11), top + SF(4))};
                gr.DrawLines(&pen, tick, 3);
            }
            wchar_t label[64];
            GetWindowTextW(d.hwndItem, label, 64);
            text(gr, label, RectF(SF(22), (r.bottom - SF(18)) / 2, static_cast<float>(r.right) - SF(22), SF(18)), 12.5f,
                 ink, false, FontStyleRegular, StringAlignmentNear);
            if (focus && !(d.itemState & ODS_NOFOCUSRECT)) {
                Pen pen(C(t.primary), 1);
                pen.SetDashStyle(DashStyleDot);
                gr.DrawRectangle(&pen, SF(20), 1.0f, SF(80), static_cast<float>(r.bottom - 3));
            }
        }
        BitBlt(d.hDC, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bitmap);
        DeleteDC(mem);
        return;
    }
    HDC mem = CreateCompatibleDC(d.hDC);
    HBITMAP bitmap = CreateCompatibleBitmap(d.hDC, r.right, r.bottom);
    HGDIOBJ old = SelectObject(mem, bitmap);
    {
        Graphics gr(mem);
        gr.SetSmoothingMode(SmoothingModeAntiAlias);
        gr.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        gr.Clear(C(id == ID_MENU ? t.background : t.panel));
        COLORREF fill = t.button, stroke = t.border, ink = t.button_text;
        if (segment) {
            fill = selected ? t.accent : t.field;
            ink = selected ? (g->phosphor == Phosphor::System ? RGB(255, 255, 255) : t.background) : t.text;
        } else if (primary && !disabled) {
            fill = t.accent;
            ink = g->phosphor == Phosphor::System ? RGB(255, 255, 255) : t.background;
            stroke = t.accent;
        } else if (id == ID_STOP && !disabled) {
            ink = t.warning;
        }
        if (pressed) fill = blend(fill, t.text, 0.15);
        if (disabled) {
            ink = blend(t.panel, ink, 0.45);
            fill = blend(t.panel, fill, 0.6);
        }
        RECT inset = {0, 0, r.right - 1, r.bottom - 1};
        rounded(gr, inset, SF(segment ? 3 : 5), fill, stroke);
        wchar_t label[64];
        GetWindowTextW(d.hwndItem, label, 64);
        text(gr, label, RectF(0, (r.bottom - SF(18)) / 2, static_cast<float>(r.right), SF(18)), 13, ink, false,
             primary ? FontStyleBold : FontStyleRegular, StringAlignmentCenter);
        if (focus && !(d.itemState & ODS_NOFOCUSRECT)) {
            Pen pen(C(t.primary), 1);
            pen.SetDashStyle(DashStyleDot);
            gr.DrawRectangle(&pen, 2, 2, r.right - 5, r.bottom - 5);
        }
    }
    BitBlt(d.hDC, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bitmap);
    DeleteDC(mem);
}

void command(int id);

// The ☰ Main Menu (spec §13 H1): every menu of the app, each as a submenu, in the Mac's order.
HMENU build_menu() {
    HMENU menu = CreatePopupMenu(), app = CreatePopupMenu(), edit = CreatePopupMenu(), camera = CreatePopupMenu(),
          view = CreatePopupMenu(), phosphor = CreatePopupMenu(), window = CreatePopupMenu(), help = CreatePopupMenu();
    bool connected = g->state == State::Connected;
    UINT on = MF_STRING, off = MF_STRING | MF_GRAYED;
    AppendMenuW(app, on, CMD_ABOUT, L"About PTZ Control");
    AppendMenuW(app, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(app, on, CMD_QUIT, L"Quit PTZ Control\tCtrl+Q");

    // Edit acts on the field that has the focus, as its own shortcuts do.
    HWND focus = GetFocus();
    wchar_t cls[32] = L"";
    if (focus) GetClassNameW(focus, cls, 32);
    bool rich = _wcsnicmp(cls, L"RichEdit", 8) == 0;
    bool text = rich || _wcsicmp(cls, L"Edit") == 0;
    bool editable = text && !(GetWindowLongW(focus, GWL_STYLE) & ES_READONLY);
    DWORD from = 0, to = 0;
    if (text) SendMessageW(focus, EM_GETSEL, reinterpret_cast<WPARAM>(&from), reinterpret_cast<LPARAM>(&to));
    bool selected = to > from;
    AppendMenuW(edit, editable && SendMessageW(focus, EM_CANUNDO, 0, 0) ? on : off, CMD_UNDO, L"Undo\tCtrl+Z");
    // Plain Windows edit fields keep one undo step and no redo; rich edit fields (the event buffer) have both.
    AppendMenuW(edit, editable && rich && SendMessageW(focus, EM_CANREDO, 0, 0) ? on : off, CMD_REDO, L"Redo\tCtrl+Shift+Z");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, editable && selected ? on : off, CMD_CUT, L"Cut\tCtrl+X");
    AppendMenuW(edit, selected ? on : off, CMD_COPY, L"Copy\tCtrl+C");
    AppendMenuW(edit, editable && IsClipboardFormatAvailable(CF_UNICODETEXT) ? on : off, CMD_PASTE, L"Paste\tCtrl+V");
    AppendMenuW(edit, text ? on : off, CMD_SELECT_ALL, L"Select All\tCtrl+A");

    AppendMenuW(camera, g->state == State::Idle || g->state == State::Connecting ? on : off, CMD_CONNECT,
                g->state == State::Connecting ? L"Cancel Connection\tCtrl+K" : L"Connect\tCtrl+K");
    AppendMenuW(camera, connected ? on : off, CMD_DISCONNECT, L"Disconnect\tCtrl+Shift+K");
    AppendMenuW(camera, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(camera, connected ? on : off, CMD_MOVE, L"Move to Target\tCtrl+Enter");
    AppendMenuW(camera, connected ? on : off, CMD_RETURN, L"Return to Starting Coordinates\tCtrl+B");
    AppendMenuW(camera, connected ? on : off, CMD_STOP, L"Stop Motion\tCtrl+.");
    AppendMenuW(camera, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(camera, connected ? on : off, CMD_REFRESH, L"Refresh Position\tCtrl+R");
    AppendMenuW(camera, connected ? on : off, CMD_PAUSE, g->paused ? L"Resume Auto-Refresh\tCtrl+P" : L"Pause Auto-Refresh\tCtrl+P");
    AppendMenuW(camera, connected ? on : off, CMD_REFRESH_PICTURE, L"Refresh Picture\tCtrl+Shift+R");
    AppendMenuW(camera, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(camera, connected ? on : off, CMD_WEB, L"Open Camera Web Interface\tCtrl+L");
    AppendMenuW(camera, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(camera, vault_state() != VaultState::Unavailable ? on : off, CMD_LOGINS, L"Manage Logins…\tCtrl+Shift+L");

    for (int i = 0; i < 5; ++i) {
        std::wstring label = std::wstring(phosphor_title(static_cast<Phosphor>(i))) + L"\tCtrl+Alt+" + std::to_wstring(i + 1);
        AppendMenuW(phosphor, MF_STRING | (static_cast<int>(g->phosphor) == i ? MF_CHECKED : 0), CMD_THEME + i, label.c_str());
    }
    AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(phosphor), L"Phosphor");

    AppendMenuW(window, on, CMD_MINIMIZE, L"Minimize\tCtrl+M");
    AppendMenuW(window, on, CMD_MAXIMIZE, IsZoomed(g->hwnd) ? L"Restore" : L"Maximize");
    AppendMenuW(window, on, CMD_CLOSE, L"Close\tCtrl+W");

    AppendMenuW(help, on, CMD_HELP, L"Coordinate Help\tF1");

    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(app), L"PTZ Control");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), L"Edit");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(camera), L"Camera");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"View");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(window), L"Window");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"Help");
    return menu;
}

void show_menu() {
    HMENU menu = build_menu();
    RECT r;
    GetWindowRect(g->menu, &r);
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_TOPALIGN, r.right, r.bottom + S(2), 0, g->hwnd, nullptr);
    DestroyMenu(menu);
}

// Test copy (PTZ_MENU_REPORT): the Main Menu's outline, its button, and whether a menu command acts.
void write_menu_report(const wchar_t *path) {
    std::wstring out = std::wstring(L"main menu button: ") + (g->menu ? L"yes" : L"no") + L"\n";
    Phosphor before = g->phosphor;
    command(CMD_THEME + 1);
    out += L"menu command acts: theme " + std::wstring(phosphor_title(before)) + L" -> " + phosphor_title(g->phosphor) + L"\n";
    command(CMD_THEME + static_cast<int>(before));
    HMENU menu = build_menu();
    std::function<void(HMENU, int)> outline = [&](HMENU m, int depth) {
        for (int i = 0, n = GetMenuItemCount(m); i < n; ++i) {
            wchar_t title[128] = L"";
            MENUITEMINFOW info{sizeof(info), MIIM_FTYPE | MIIM_STRING | MIIM_SUBMENU};
            info.dwTypeData = title;
            info.cch = 127;
            GetMenuItemInfoW(m, i, TRUE, &info);
            if (info.fType & MFT_SEPARATOR) continue;
            std::wstring t = title;
            out += std::wstring(depth * 2, L' ') + t.substr(0, t.find(L'\t')) + L"\n";
            if (info.hSubMenu) outline(info.hSubMenu, depth + 1);
        }
    };
    outline(menu, 0);
    DestroyMenu(menu);
    std::string utf8(WideCharToMultiByte(CP_UTF8, 0, out.c_str(), -1, nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, out.c_str(), -1, utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
    utf8.pop_back();
    if (FILE *f = _wfopen(path, L"wb")) {
        fwrite(utf8.data(), 1, utf8.size(), f);
        fclose(f);
    }
}

void command(int id) {
    switch (id) {
        case ID_ROUTE_LOCAL: case ID_ROUTE_SSH:
            if (g->state != State::Idle) return;
            g->route = id == ID_ROUTE_SSH;
            layout();
            InvalidateRect(g->route_local, nullptr, FALSE);
            InvalidateRect(g->route_ssh, nullptr, FALSE);
            return;
        case ID_VENDOR_ONVIF: case ID_VENDOR_AXIS:
            if (g->state != State::Idle) return;
            g->vendor = id == ID_VENDOR_AXIS;
            InvalidateRect(g->vendor_onvif, nullptr, FALSE);
            InvalidateRect(g->vendor_axis, nullptr, FALSE);
            InvalidateRect(g->hwnd, &g->vendor_note, FALSE);
            return;
        case ID_CONNECT: case CMD_CONNECT: toggle_connection(); return;
        case CMD_DISCONNECT: disconnect(); return;
        case ID_MOVE: case CMD_MOVE: send_move(); return;
        case ID_START: case CMD_RETURN: return_to_start(); return;
        case ID_STOP: case CMD_STOP: stop_motion(); return;
        case ID_WEB: case CMD_WEB: open_web(); return;
        case CMD_REFRESH: refresh_now(); return;
        case CMD_LOGINS: show_manage_logins(g->hwnd); return;
        case ID_SAVE_SSH: case ID_SAVE_CAM:
            if (g->state != State::Idle) return;
            (id == ID_SAVE_SSH ? g->save_ssh_on : g->save_cam_on) ^= true;
            InvalidateRect(id == ID_SAVE_SSH ? g->save_ssh : g->save_cam, nullptr, FALSE);
            return;
        case CMD_PAUSE: toggle_pause(); return;
        case ID_REFRESH_PICTURE: case CMD_REFRESH_PICTURE: take_picture(); return;
        case CMD_QUIT: case CMD_CLOSE: PostMessageW(g->hwnd, WM_CLOSE, 0, 0); return;
        case CMD_MINIMIZE: ShowWindow(g->hwnd, SW_MINIMIZE); return;
        case CMD_MAXIMIZE: ShowWindow(g->hwnd, IsZoomed(g->hwnd) ? SW_RESTORE : SW_MAXIMIZE); return;
        case CMD_UNDO: case CMD_CUT: case CMD_COPY: case CMD_PASTE: case CMD_SELECT_ALL: {
            HWND focus = GetFocus();
            if (!focus) return;
            if (id == CMD_SELECT_ALL) SendMessageW(focus, EM_SETSEL, 0, -1);
            else SendMessageW(focus, id == CMD_UNDO ? WM_UNDO : id == CMD_CUT ? WM_CUT : id == CMD_COPY ? WM_COPY : WM_PASTE, 0, 0);
            return;
        }
        case CMD_REDO: if (HWND focus = GetFocus()) SendMessageW(focus, EM_REDO, 0, 0); return;
        case ID_MENU: show_menu(); return;
        case CMD_HELP: show_help(); return;
        case CMD_ABOUT: {
            // A task dialog, unlike a message box, can hold a link; clicking it opens the browser.
            const std::wstring version = L"PTZ Control " + W(ptz_core_version());
            TASKDIALOGCONFIG about{};
            about.cbSize = sizeof(about);
            about.hwndParent = g->hwnd;
            about.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION;
            about.dwCommonButtons = TDCBF_OK_BUTTON;
            about.pszWindowTitle = L"About PTZ Control";
            about.pszMainIcon = TD_INFORMATION_ICON;
            about.pszMainInstruction = version.c_str();
            about.pszContent = L"PTZ Coordinates Controller\nONVIF \u00b7 Axis VAPIX \u00b7 Local or SSH\n\nBy dunkelherzmann\n"
                               L"<a href=\"https://github.com/dunkelherzmann/ptz-control\">https://github.com/dunkelherzmann/ptz-control</a>";
            about.pfCallback = [](HWND window, UINT notification, WPARAM, LPARAM lParam, LONG_PTR) -> HRESULT {
                if (notification == TDN_HYPERLINK_CLICKED)
                    ShellExecuteW(window, L"open", reinterpret_cast<LPCWSTR>(lParam), nullptr, nullptr, SW_SHOWNORMAL);
                return S_OK;
            };
            TaskDialogIndirect(&about, nullptr, nullptr, nullptr);
            return;
        }
        case IDOK: {
            // Enter: move from a target field, otherwise connect while idle.
            HWND focus = GetFocus();
            if (std::find(g->target_fields.begin(), g->target_fields.end(), focus) != g->target_fields.end()) send_move();
            else if (g->state == State::Idle) connect();
            return;
        }
    }
    if (id >= CMD_THEME && id < CMD_THEME + 5) {
        g->phosphor = static_cast<Phosphor>(id - CMD_THEME);
        g->settings.theme = id - CMD_THEME;
        g->settings.save();
        apply_theme();
    }
}

HWND make_button(const wchar_t *label, int id) {
    return CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, g->hwnd,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g->instance, nullptr);
}

HWND make_edit(int id, const std::wstring &value, const wchar_t *cue, DWORD extra = 0) {
    HWND e = CreateWindowExW(0, L"EDIT", value.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra, 0, 0,
                             10, 10, g->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g->instance, nullptr);
    if (cue) SendMessageW(e, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));
    return e;
}

void create_children() {
    const Settings &s = g->settings;
    g->route = s.route;
    g->vendor = s.vendor;
    g->route_local = make_button(L"Local", ID_ROUTE_LOCAL);
    g->route_ssh = make_button(L"SSH Host", ID_ROUTE_SSH);
    g->vendor_onvif = make_button(L"Bosch/Milesight", ID_VENDOR_ONVIF);
    g->vendor_axis = make_button(L"Axis", ID_VENDOR_AXIS);

    g->ssh_host = make_edit(ID_SSH_HOST, s.ssh_host, L"jump.example.com");
    g->ssh_user = make_edit(ID_SSH_USER, s.ssh_user, L"username");
    g->ssh_password = make_edit(ID_SSH_PASSWORD, L"", L"blank for keys / agent", ES_PASSWORD);
    g->ssh_port = make_edit(ID_SSH_PORT, std::to_wstring(s.ssh_port), nullptr, ES_NUMBER);
    g->ssh_key = make_edit(ID_SSH_KEY, s.ssh_identity, L"optional, e.g. C:\\Users\\me\\.ssh\\id_ed25519");
    const char *env_password = std::getenv("PTZ_PASSWORD");
    g->cam_host = make_edit(ID_CAM_HOST, s.camera_host, L"192.168.1.100");
    g->cam_port = make_edit(ID_CAM_PORT, std::to_wstring(s.camera_port), nullptr, ES_NUMBER);
    g->cam_user = make_edit(ID_CAM_USER, s.camera_user, nullptr);
    g->cam_password = make_edit(ID_CAM_PASSWORD, env_password ? W(env_password) : L"", L"required", ES_PASSWORD);
    g->https_port = make_edit(ID_HTTPS_PORT, std::to_wstring(s.https_port), nullptr, ES_NUMBER);
    g->profile = make_edit(ID_PROFILE, std::to_wstring(s.profile), nullptr, ES_NUMBER);
    g->timeout = make_edit(ID_TIMEOUT, format_value(s.timeout), nullptr);
    auto combo = [](int id) {
        return CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                               0, 0, 10, 200, g->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g->instance, nullptr);
    };
    g->ssh_login = combo(ID_SSH_LOGIN);
    g->cam_login = combo(ID_CAM_LOGIN);
    g->save_ssh = make_button(L"Save login", ID_SAVE_SSH);
    g->save_cam = make_button(L"Save login", ID_SAVE_CAM);
    g->rows = {
        {L"Login", g->ssh_login, {}, true, kCombo},
        {L"Host", g->ssh_host, {}, true}, {L"Username", g->ssh_user, {}, true}, {L"Password", g->ssh_password, {}, true},
        {L"Port", g->ssh_port, {}, true}, {L"Key file", g->ssh_key, {}, true}, {L"", g->save_ssh, {}, true, kCheck},
        {L"Login", g->cam_login, {}, false, kCombo},
        {L"Address", g->cam_host, {}, false}, {L"HTTP port", g->cam_port, {}, false},
        {L"Username", g->cam_user, {}, false}, {L"Password", g->cam_password, {}, false},
        {L"", g->save_cam, {}, false, kCheck},
        {L"HTTPS port", g->https_port, {}, false}, {L"Profile", g->profile, {}, false},
        {L"Timeout (s)", g->timeout, {}, false},
    };
    g->connect = make_button(L"Connect", ID_CONNECT);

    g->menu = make_button(L"\u2630", ID_MENU);
    g->plane = create_plane(g->hwnd, ID_PLANE);
    g->meter_pan = create_meter(g->hwnd, ID_METER_PAN, L"PAN");
    g->meter_tilt = create_meter(g->hwnd, ID_METER_TILT, L"TILT");
    g->meter_zoom = create_meter(g->hwnd, ID_METER_ZOOM, L"ZOOM");
    g->target_pan = make_edit(ID_TARGET_PAN, L"", L"Pan: unchanged");
    g->target_tilt = make_edit(ID_TARGET_TILT, L"", L"Tilt: unchanged");
    g->target_zoom = make_edit(ID_TARGET_ZOOM, L"", L"Zoom: unchanged");
    g->target_fields = {g->target_pan, g->target_tilt, g->target_zoom};
    g->move = make_button(L"Move", ID_MOVE);
    g->start = make_button(L"Return to Start", ID_START);
    g->stop = make_button(L"Stop", ID_STOP);
    g->web = make_button(L"Web Interface", ID_WEB);
    g->log = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 10,
                             10, g->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_LOG)), g->instance, nullptr);

    g->refresh_picture = make_button(L"Refresh Picture", ID_REFRESH_PICTURE);
    PlaneState &plane = plane_state(g->plane);
    plane.on_picture_aim = [](float x, float y) { aim_in_picture(x, y); };
    plane.on_picture_aim_and_move = [](float x, float y) {
        aim_in_picture(x, y);
        send_move();
    };
    for (auto [meter, field] : {std::pair{g->meter_pan, g->target_pan}, std::pair{g->meter_tilt, g->target_tilt},
                                std::pair{g->meter_zoom, g->target_zoom}}) {
        MeterState &m = meter_state(meter);
        HWND f = field;
        m.on_aim = [f](double v) {
            SetWindowTextW(f, format_value(v).c_str());
            update_target_markers();
        };
        m.on_aim_and_move = [f](double v) {
            SetWindowTextW(f, format_value(v).c_str());
            send_move();
        };
    }

    ACCEL keys[] = {
        {FCONTROL | FVIRTKEY, 'K', CMD_CONNECT}, {FCONTROL | FSHIFT | FVIRTKEY, 'K', CMD_DISCONNECT},
        {FCONTROL | FSHIFT | FVIRTKEY, 'L', CMD_LOGINS},
        {FCONTROL | FVIRTKEY, VK_RETURN, CMD_MOVE}, {FCONTROL | FVIRTKEY, 'B', CMD_RETURN},
        {FCONTROL | FVIRTKEY, VK_OEM_PERIOD, CMD_STOP}, {FCONTROL | FVIRTKEY, 'R', CMD_REFRESH},
        {FCONTROL | FVIRTKEY, 'P', CMD_PAUSE}, {FCONTROL | FSHIFT | FVIRTKEY, 'R', CMD_REFRESH_PICTURE}, {FCONTROL | FVIRTKEY, 'L', CMD_WEB}, {FVIRTKEY, VK_F1, CMD_HELP},
        {FCONTROL | FALT | FVIRTKEY, '1', CMD_THEME + 0}, {FCONTROL | FALT | FVIRTKEY, '2', CMD_THEME + 1},
        {FCONTROL | FALT | FVIRTKEY, '3', CMD_THEME + 2}, {FCONTROL | FALT | FVIRTKEY, '4', CMD_THEME + 3},
        {FCONTROL | FALT | FVIRTKEY, '5', CMD_THEME + 4},
        {FCONTROL | FVIRTKEY, 'Q', CMD_QUIT}, {FCONTROL | FVIRTKEY, 'M', CMD_MINIMIZE}, {FCONTROL | FVIRTKEY, 'W', CMD_CLOSE},
    };
    g->accelerators = CreateAcceleratorTableW(keys, static_cast<int>(std::size(keys)));

    g->tips = create_tooltips(g->hwnd);
    add_tooltip(g->tips, g->ssh_login,
                L"SSH login \u2014 Fill the SSH username and password from a saved SSH login, or open Manage Logins "
                L"(Ctrl+Shift+L).");
    add_tooltip(g->tips, g->cam_login,
                L"Camera login \u2014 Fill the camera username and password from a saved camera login, or open Manage "
                L"Logins (Ctrl+Shift+L).");
    add_tooltip(g->tips, g->save_ssh,
                L"Save login \u2014 Once connected, save this SSH username and password to the saved SSH logins.");
    add_tooltip(g->tips, g->refresh_picture, L"Refresh Picture: take a new picture of the camera's view now (Ctrl+Shift+R)");
    add_tooltip(g->tips, g->save_cam,
                L"Save login \u2014 Once connected, save this camera username and password to the saved camera logins.");
    reload_logins();
    vault_subscribe([] { reload_logins(); });

    link_set_delegate(g->link, LinkDelegate{
        [](const std::wstring &line) { log_line(line); },
        [](const std::wstring &message, bool secret) { return run_prompt(message, secret); },
        [](const std::wstring &title, const std::wstring &message) {
            return MessageBoxW(g->hwnd, message.c_str(), title.c_str(), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
        },
    });
}

// ---- UI test hook (PTZ_SNAPSHOT_PATH) -----------------------------------------------------
// Like the Mac and Linux apps: connect with the saved settings (PTZ_PASSWORD), move, aim along the zoom
// meter, save a picture of the window and close. PTZ_SNAPSHOT_SAVE_LOGIN ticks the camera's Save login box,
// PTZ_SNAPSHOT_USE_LOGIN fills the camera fields from the first saved camera login, and
// PTZ_SNAPSHOT_LOGINS_PATH also saves a picture of Manage Logins. The window never takes focus.

enum : int { kSnapStart = 50, kSnapMove, kSnapAim, kSnapCapture, kPicAim, kPicMove, kPicMoving, kPicCapture };

bool save_png(HWND window, const std::wstring &path) {
    RECT r;
    GetWindowRect(window, &r);
    int w = r.right - r.left, h = r.bottom - r.top;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bitmap);
    // WM_PRINT has each window paint itself into the bitmap, so it works without a compositor too
    // (a test run over SSH, in session 0).
    SendMessageW(window, WM_PRINT, reinterpret_cast<WPARAM>(mem), PRF_NONCLIENT | PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
    SelectObject(mem, old);
    bool saved = false;
    {
        UINT count = 0, size = 0;
        GetImageEncodersSize(&count, &size);
        std::vector<BYTE> buffer(size);
        auto *codecs = reinterpret_cast<ImageCodecInfo *>(buffer.data());
        GetImageEncoders(count, size, codecs);
        for (UINT i = 0; i < count; ++i)
            if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
                Bitmap picture(bitmap, nullptr);
                saved = picture.Save(path.c_str(), &codecs[i].Clsid, nullptr) == Ok;
            }
    }
    DeleteObject(bitmap);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return saved;
}

void snapshot_step(int step) {
    switch (step) {
        case kSnapStart:
            if (const wchar_t *menus = _wgetenv(L"PTZ_MENU_REPORT")) write_menu_report(menus);
            if (_wgetenv(L"PTZ_SNAPSHOT_SAVE_LOGIN")) g->save_cam_on = true;
            if (_wgetenv(L"PTZ_SNAPSHOT_USE_LOGIN")) choose_saved_login(LoginStore::Camera, 0);
            connect();
            // PTZ_SNAPSHOT_PICTURE: aim at a point of the picture (a picture of that is saved to PTZ_SNAPSHOT_AIM_PATH),
            // move there, and save the window once the new picture has come.
            SetTimer(g->hwnd, _wgetenv(L"PTZ_SNAPSHOT_PICTURE") ? kPicAim : kSnapMove, 3000, nullptr);
            return;
        case kPicAim:
            if (plane_state(g->plane).on_picture_aim) plane_state(g->plane).on_picture_aim(0.45f, 0.3f);
            SetTimer(g->hwnd, kPicMove, 400, nullptr);
            return;
        case kPicMove:
            if (const wchar_t *path = _wgetenv(L"PTZ_SNAPSHOT_AIM_PATH")) save_png(g->hwnd, path);
            send_move();
            SetTimer(g->hwnd, kPicMoving, 1000, nullptr);
            return;
        case kPicMoving:
            if (const wchar_t *path = _wgetenv(L"PTZ_SNAPSHOT_MOVING_PATH")) save_png(g->hwnd, path);
            SetTimer(g->hwnd, kPicCapture, 5600, nullptr);
            return;
        case kPicCapture:
            save_png(g->hwnd, _wgetenv(L"PTZ_SNAPSHOT_PATH"));
            PostMessageW(g->hwnd, WM_CLOSE, 0, 0);
            return;
        case kSnapMove:
            SetWindowTextW(g->target_pan, L"0.55");
            SetWindowTextW(g->target_tilt, L"0.22");
            SetWindowTextW(g->target_zoom, L"0.4");
            send_move();
            SetTimer(g->hwnd, kSnapAim, 1200, nullptr);
            return;
        case kSnapAim:
            SetWindowTextW(g->target_pan, L"-0.3");
            SetWindowTextW(g->target_tilt, L"0.6");
            if (meter_state(g->meter_zoom).on_aim) meter_state(g->meter_zoom).on_aim(0.75);
            SetTimer(g->hwnd, kSnapCapture, 2800, nullptr);
            return;
        case kSnapCapture: {
            // What each Login menu shows, for the test log (a combo box draws its text only on screen).
            for (HWND box : {g->ssh_login, g->cam_login}) {
                wchar_t shown[128] = L"";
                GetWindowTextW(box, shown, 128);
                log_line(std::wstring(box == g->ssh_login ? L"Test: SSH Login menu shows \u201c" : L"Test: camera Login menu shows \u201c") +
                         shown + L"\u201d");
            }
            save_png(g->hwnd, _wgetenv(L"PTZ_SNAPSHOT_PATH"));
            if (const wchar_t *logins = _wgetenv(L"PTZ_SNAPSHOT_LOGINS_PATH")) {
                HWND window = manage_logins_for_snapshot(g->hwnd, LoginStore::Camera);
                ShowWindow(window, SW_SHOWNOACTIVATE);
                UpdateWindow(window);
                save_png(window, logins);
                manage_logins_close_snapshot();
            }
            PostMessageW(g->hwnd, WM_CLOSE, 0, 0);
            return;
        }
    }
}

LRESULT CALLBACK console_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g->hwnd = hwnd;
            g->dpi = GetDpiForWindow(hwnd);
            g->link = create_link(hwnd);
            create_children();
            make_fonts();
            SendMessageW(g->log, EM_SETTARGETDEVICE, 0, 0);  // wrap to the window width
            apply_theme();
            layout();
            update_controls();
            log_line(L"PTZ Control ready \u00b7 core " + W(ptz_core_version()));
            if (vault_state() == VaultState::Unavailable) log_line(L"Saved logins are unavailable: " + vault_open_error(), true);
            SetTimer(hwnd, 1, 250, nullptr);
            if (_wgetenv(L"PTZ_SNAPSHOT_PATH")) SetTimer(hwnd, kSnapStart, 300, nullptr);
            return 0;
        }
        case WM_APP_CALL: {
            std::unique_ptr<std::function<void()>> fn(reinterpret_cast<std::function<void()> *>(lp));
            (*fn)();
            return 0;
        }
        case WM_APP_SYNC:
            (*reinterpret_cast<std::function<void()> *>(lp))();
            return 0;
        case WM_TIMER:
            if (wp >= kSnapStart && wp <= kPicCapture) {
                KillTimer(hwnd, wp);
                snapshot_step(static_cast<int>(wp));
                return 0;
            }
            tick();
            return 0;
        case WM_SIZE:
            layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto *mm = reinterpret_cast<MINMAXINFO *>(lp);
            UINT dpi = g && g->hwnd ? g->dpi : GetDpiForWindow(hwnd);
            mm->ptMinTrackSize = {MulDiv(1020, static_cast<int>(dpi), 96), MulDiv(780, static_cast<int>(dpi), 96)};
            return 0;
        }
        case WM_DPICHANGED: {
            g->dpi = HIWORD(wp);
            instruments_set_theme(g->theme, g->dpi);
            make_fonts();
            auto *r = reinterpret_cast<RECT *>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (g->phosphor == Phosphor::System) apply_theme();  // Windows light/dark switch
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PRINTCLIENT: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            paint(reinterpret_cast<HDC>(wp), rc);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom));
            HGDIOBJ old = SelectObject(mem, bitmap);
            paint(mem, rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bitmap);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, IsWindowEnabled(reinterpret_cast<HWND>(lp)) ? g->theme.field_text
                                                                         : blend(g->theme.field, g->theme.field_text, 0.55));
            SetBkColor(dc, g->theme.field);
            return reinterpret_cast<LRESULT>(g->field_brush);
        }
        case WM_DRAWITEM:
            draw_button(*reinterpret_cast<DRAWITEMSTRUCT *>(lp));
            return TRUE;
        case WM_COMMAND:
            if (lp && HIWORD(wp) == CBN_SELCHANGE &&
                (reinterpret_cast<HWND>(lp) == g->ssh_login || reinterpret_cast<HWND>(lp) == g->cam_login)) {
                login_chosen(reinterpret_cast<HWND>(lp));
                return 0;
            }
            if (HIWORD(wp) == EN_CHANGE) {
                HWND from = reinterpret_cast<HWND>(lp);
                if (std::find(g->target_fields.begin(), g->target_fields.end(), from) != g->target_fields.end()) {
                    update_target_markers();
                    // Typing an absolute target takes over from a point of the picture.
                    if (GetFocus() == from && plane_state(g->plane).picture_target) {
                        plane_state(g->plane).picture_target.reset();
                        InvalidateRect(g->plane, nullptr, FALSE);
                    }
                }
                // Typing a username or password means it's no longer the saved login.
                if (!g->filling && (from == g->ssh_user || from == g->ssh_password)) SendMessageW(g->ssh_login, CB_SETCURSEL, 0, 0);
                if (!g->filling && (from == g->cam_user || from == g->cam_password)) SendMessageW(g->cam_login, CB_SETCURSEL, 0, 0);
                return 0;
            }
            command(LOWORD(wp));
            return 0;
        case WM_CLOSE:
            if (g->state == State::Connected || g->state == State::Connecting) {
                if (!g->closing) {
                    g->closing = true;
                    disconnect([hwnd] { DestroyWindow(hwnd); });
                }
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            for (HWND p : {g->cam_password, g->ssh_password}) {
                wipe(p);
                SetWindowTextW(p, L"");
            }
            destroy_link(g->link);
            g->link = nullptr;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

HWND create_console(HINSTANCE instance, int show) {
    g = new Console;
    g->instance = instance;
    g->settings = Settings::load();
    g->phosphor = static_cast<Phosphor>(g->settings.theme);
    LoadLibraryW(L"Msftedit.dll");
    register_instrument_classes(instance);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = console_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), 0));
    wc.lpszClassName = L"PTZConsole";
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"PTZConsole", L"PTZ Control",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                MulDiv(1180, static_cast<int>(dpi), 96), MulDiv(820, static_cast<int>(dpi), 96), nullptr,
                                nullptr, instance, nullptr);
    // A snapshot test copy never takes focus from the desktop the user is working on.
    ShowWindow(hwnd, _wgetenv(L"PTZ_SNAPSHOT_PATH") ? SW_SHOWNOACTIVATE : show);
    fit_window_to_picture();
    UpdateWindow(hwnd);
    return hwnd;
}

bool console_translate(MSG &msg) {
    if (!g || !g->hwnd) return false;
    if (g->accelerators && TranslateAcceleratorW(g->hwnd, g->accelerators, &msg)) return true;
    // Tab/Shift+Tab between fields and Enter as IDOK, like a dialog.
    if (msg.hwnd != g->log && IsDialogMessageW(g->hwnd, &msg)) return true;
    return false;
}
