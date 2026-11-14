// Text helpers, themes, registry settings and the camera-link worker.
#include <cstdlib>
#include "app.h"

#include <gdiplus.h>
#include <shlwapi.h>

#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

// ---- text --------------------------------------------------------------------

std::wstring W(const std::string &utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string U(const std::wstring &wide) {
    if (wide.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring format_value(double value) {
    char buffer[64];
    if (value == static_cast<double>(static_cast<long long>(value)) && value > -1e15 && value < 1e15) {
        std::snprintf(buffer, sizeof buffer, "%lld", static_cast<long long>(value));
        return W(buffer);
    }
    // The C locale is never changed in this process, so "%f" always uses '.'.
    std::snprintf(buffer, sizeof buffer, "%.4f", value);
    std::string text = buffer;
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    return W(text);
}

bool parse_value(const std::wstring &text, double &value) { return ptz_parse_number(U(text).c_str(), &value) != 0; }

bool Snapshot::operator==(const Snapshot &o) const {
    auto same = [](const std::optional<Reading> &a, const std::optional<Reading> &b) {
        return a.has_value() == b.has_value() && (!a || a->text == b->text);
    };
    return same(pan, o.pan) && same(tilt, o.tilt) && same(zoom, o.zoom) && move_status == o.move_status;
}

// ---- theme -------------------------------------------------------------------

const wchar_t *phosphor_title(Phosphor p) {
    switch (p) {
        case Phosphor::System: return L"System";
        case Phosphor::Green: return L"Green Phosphor";
        case Phosphor::Amber: return L"Amber Phosphor";
        case Phosphor::Blue: return L"Blue Phosphor";
        case Phosphor::Mono: return L"Mono";
    }
    return L"System";
}

COLORREF blend(COLORREF a, COLORREF b, double t) {
    auto mix = [t](int x, int y) { return static_cast<BYTE>(x + (y - x) * t + 0.5); };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

static bool windows_apps_dark() {
    DWORD value = 1, size = sizeof value;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0;
}

Theme make_theme(Phosphor p) {
    Theme t{};
    if (p == Phosphor::System) {
        COLORREF accent = GetSysColor(COLOR_HIGHLIGHT);
        t.dark = windows_apps_dark();
        if (t.dark) {
            t.background = RGB(32, 32, 32);
            t.panel = RGB(43, 43, 43);
            t.border = RGB(70, 70, 70);
            t.text = RGB(240, 240, 240);
            t.secondary = RGB(160, 160, 160);
            t.field = RGB(30, 30, 30);
            t.button = RGB(60, 60, 60);
        } else {
            t.background = RGB(243, 243, 243);
            t.panel = RGB(255, 255, 255);
            t.border = RGB(214, 214, 214);
            t.text = RGB(26, 26, 26);
            t.secondary = RGB(100, 100, 100);
            t.field = RGB(250, 250, 250);
            t.button = RGB(232, 232, 232);
        }
        t.primary = accent;
        t.accent = accent;
        t.grid = blend(t.panel, t.secondary, 0.18);
        t.axis = blend(t.panel, t.secondary, 0.55);
        t.start = RGB(230, 140, 30);
        t.target = RGB(226, 60, 120);
        t.warning = t.dark ? RGB(255, 99, 90) : RGB(196, 43, 28);
        t.good = t.dark ? RGB(108, 203, 95) : RGB(16, 124, 16);
        t.field_text = t.text;
        t.button_text = t.text;
        return t;
    }
    COLORREF ph = p == Phosphor::Green ? RGB(0x3D, 0xFF, 0x86)
                : p == Phosphor::Amber ? RGB(0xFF, 0xB3, 0x3D)
                : p == Phosphor::Blue  ? RGB(0x5C, 0xC8, 0xFF)
                                       : RGB(0xE6, 0xE6, 0xE6);
    t.dark = true;
    t.background = RGB(7, 9, 10);
    t.panel = RGB(13, 17, 18);
    t.border = blend(t.panel, ph, 0.28);
    t.grid = blend(t.panel, ph, 0.12);
    t.axis = blend(t.panel, ph, 0.45);
    t.primary = ph;
    t.secondary = blend(t.panel, ph, 0.6);
    t.text = blend(ph, RGB(255, 255, 255), 0.55);
    t.start = blend(t.panel, ph, 0.7);
    t.target = blend(ph, RGB(255, 255, 255), 0.5);
    t.warning = RGB(0xFF, 0x5A, 0x4F);
    t.good = ph;
    t.field = RGB(4, 6, 7);
    t.field_text = t.text;
    t.button = blend(t.panel, ph, 0.14);
    t.button_text = t.text;
    t.accent = blend(t.panel, ph, 0.55);
    return t;
}

// ---- settings ----------------------------------------------------------------

// PTZ_SETTINGS_KEY (under HKCU) gives a test copy its own settings.
static const wchar_t *settings_key() {
    static const std::wstring key = [] {
        const wchar_t *v = _wgetenv(L"PTZ_SETTINGS_KEY");
        return std::wstring(v && *v ? v : L"Software\\PTZControl");
    }();
    return key.c_str();
}

static std::wstring reg_string(HKEY key, const wchar_t *name, const std::wstring &fallback) {
    wchar_t buffer[1024];
    DWORD size = sizeof buffer;
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) return fallback;
    return buffer;
}

static int reg_int(HKEY key, const wchar_t *name, int fallback) {
    DWORD value = 0, size = sizeof value;
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) return fallback;
    return static_cast<int>(value);
}

Settings Settings::load() {
    Settings s;
    if (const char *user = std::getenv("PTZ_USER")) s.camera_user = W(user);
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, settings_key(), 0, KEY_READ, &key) != ERROR_SUCCESS) return s;
    s.route = reg_int(key, L"Route", s.route) ? 1 : 0;
    s.vendor = reg_int(key, L"Vendor", s.vendor) ? 1 : 0;
    s.camera_host = reg_string(key, L"CameraHost", s.camera_host);
    s.camera_port = reg_int(key, L"CameraPort", s.camera_port);
    s.camera_user = reg_string(key, L"CameraUser", s.camera_user);
    s.https_port = reg_int(key, L"HttpsPort", s.https_port);
    s.profile = reg_int(key, L"Profile", s.profile);
    double timeout;
    if (parse_value(reg_string(key, L"Timeout", L""), timeout) && timeout > 0) s.timeout = timeout;
    s.ssh_host = reg_string(key, L"SshHost", s.ssh_host);
    s.ssh_user = reg_string(key, L"SshUser", s.ssh_user);
    s.ssh_identity = reg_string(key, L"SshIdentity", s.ssh_identity);
    s.ssh_port = reg_int(key, L"SshPort", s.ssh_port);
    s.theme = reg_int(key, L"Theme", s.theme);
    if (s.theme < 0 || s.theme > 4) s.theme = 0;
    RegCloseKey(key);
    return s;
}

void Settings::save() const {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, settings_key(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    auto put_string = [&](const wchar_t *name, const std::wstring &v) {
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(v.c_str()),
                       static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    };
    auto put_int = [&](const wchar_t *name, int v) {
        DWORD d = static_cast<DWORD>(v);
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&d), sizeof d);
    };
    put_int(L"Route", route);
    put_int(L"Vendor", vendor);
    put_string(L"CameraHost", camera_host);
    put_int(L"CameraPort", camera_port);
    put_string(L"CameraUser", camera_user);
    put_int(L"HttpsPort", https_port);
    put_int(L"Profile", profile);
    put_string(L"Timeout", format_value(timeout));
    put_string(L"SshHost", ssh_host);
    put_string(L"SshUser", ssh_user);
    put_string(L"SshIdentity", ssh_identity);
    put_int(L"SshPort", ssh_port);
    put_int(L"Theme", theme);
    RegCloseKey(key);
}

// ---- camera link -------------------------------------------------------------

class CameraLink {
public:
    explicit CameraLink(HWND notify) : notify_(notify), thread_([this] { loop(); }) {}

    ~CameraLink() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        ready_.notify_all();
        thread_.join();
        if (session_) ptz_close(session_);
    }

    // UI thread: queue work; `done` runs back on the UI thread.
    void run(std::function<std::function<void()>()> work) {
        ++pending_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.push_back([this, work] {
                std::function<void()> done = work();
                post([this, done] {
                    --pending_;
                    done();
                });
            });
        }
        ready_.notify_one();
    }

    void post(std::function<void()> fn) {
        PostMessageW(notify_, WM_APP_CALL, 0, reinterpret_cast<LPARAM>(new std::function<void()>(std::move(fn))));
    }

    // Core threads: run on the UI thread and wait (modal prompts).
    void sync(std::function<void()> fn) { SendMessageW(notify_, WM_APP_SYNC, 0, reinterpret_cast<LPARAM>(&fn)); }

    LinkDelegate delegate;
    ptz_session *session_ = nullptr;  // worker thread only
    volatile int cancel_ = 0;
    int pending_ = 0;                 // UI thread only

private:
    void loop() {
        while (true) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
                if (jobs_.empty()) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job();
        }
    }

    HWND notify_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> jobs_;
    bool quit_ = false;
    std::thread thread_;
};

CameraLink *create_link(HWND notify) { return new CameraLink(notify); }
void destroy_link(CameraLink *link) { delete link; }
void link_set_delegate(CameraLink *link, LinkDelegate delegate) { link->delegate = std::move(delegate); }
bool link_busy(CameraLink *link) { return link->pending_ > 0; }
void link_cancel_connect(CameraLink *link) { link->cancel_ = 1; }

namespace {

std::wstring text_of(const char *buffer) { return W(buffer); }

template <size_t N>
std::wstring field(const char (&buffer)[N]) {
    return W(std::string(buffer, strnlen(buffer, N)));
}

void on_log(void *context, const char *line) {
    auto *link = static_cast<CameraLink *>(context);
    std::wstring text = W(line);
    link->post([link, text] {
        if (link->delegate.log) link->delegate.log(text);
    });
}

int on_prompt(void *context, const char *message, int secret, char *answer, size_t size) {
    auto *link = static_cast<CameraLink *>(context);
    std::optional<std::wstring> reply;
    std::wstring text = W(message);
    link->sync([&] {
        if (link->delegate.prompt) reply = link->delegate.prompt(text, secret != 0);
    });
    if (!reply) return 0;
    std::string utf8 = U(*reply);
    if (utf8.size() >= size) utf8.resize(size - 1);
    std::memcpy(answer, utf8.c_str(), utf8.size() + 1);
    SecureZeroMemory(utf8.data(), utf8.size());
    return 1;
}

int on_confirm(void *context, const char *title, const char *message) {
    auto *link = static_cast<CameraLink *>(context);
    bool yes = false;
    std::wstring t = W(title), m = W(message);
    link->sync([&] {
        if (link->delegate.confirm) yes = link->delegate.confirm(t, m);
    });
    return yes ? 1 : 0;
}

LinkResult failure(ptz_session *session, const char *error) {
    LinkResult r;
    r.error = text_of(error);
    r.link_lost = session && !ptz_link_alive(session);
    return r;
}

std::wstring executable_path() {
    std::wstring path(MAX_PATH, L'\0');
    while (true) {
        DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

}  // namespace

void link_connect(CameraLink *link, const Settings &s, const std::wstring &camera_password,
                  const std::wstring &ssh_password, std::function<void(LinkResult, CameraInfo)> done) {
    link->cancel_ = 0;
    std::string askpass = U(executable_path());
    std::string cam_pw = U(camera_password), ssh_pw = U(ssh_password);
    link->run([=]() mutable -> std::function<void()> {
        std::string camera_host = U(s.camera_host), camera_user = U(s.camera_user);
        std::string ssh_host = U(s.ssh_host), ssh_user = U(s.ssh_user), identity = U(s.ssh_identity);
        ptz_connect_params p{};
        p.route = s.route ? PTZ_ROUTE_SSH : PTZ_ROUTE_LOCAL;
        p.vendor = s.vendor ? PTZ_VENDOR_AXIS : PTZ_VENDOR_ONVIF;
        p.camera_host = camera_host.c_str();
        p.camera_port = s.camera_port;
        p.camera_user = camera_user.c_str();
        p.camera_password = cam_pw.c_str();
        p.https_port = s.https_port;
        p.profile = s.profile;
        p.timeout = s.timeout;
        p.ssh_host = ssh_host.c_str();
        p.ssh_user = ssh_user.c_str();
        p.ssh_password = ssh_pw.c_str();
        p.ssh_port = s.ssh_port;
        p.ssh_identity = identity.c_str();
        p.askpass_path = askpass.c_str();
        ptz_callbacks callbacks{link, on_log, on_prompt, on_confirm};
        char error[1024] = {};
        ptz_session *session = ptz_connect(&p, &callbacks, &link->cancel_, error, sizeof error);
        SecureZeroMemory(cam_pw.data(), cam_pw.size());
        SecureZeroMemory(ssh_pw.data(), ssh_pw.size());
        LinkResult result;
        CameraInfo info;
        if (!session) {
            result = failure(nullptr, error);
        } else {
            link->session_ = session;
            ptz_info raw;
            ptz_info_get(session, &raw);
            auto range = [](const ptz_range &r) { return AxisRange{r.available != 0, r.min, r.max}; };
            info.name = field(raw.name);
            info.units = field(raw.units);
            info.pan = range(raw.pan);
            info.tilt = range(raw.tilt);
            info.zoom = range(raw.zoom);
            info.zoom_integer = raw.zoom_integer != 0;
            info.view_moves = raw.view_moves != 0;
            result.ok = true;
        }
        return [done, result, info] { done(result, info); };
    });
}

void link_position(CameraLink *link, std::function<void(LinkResult, Snapshot)> done) {
    link->run([link, done]() -> std::function<void()> {
        LinkResult result;
        Snapshot snapshot;
        if (!link->session_) {
            result.error = L"Not connected";
        } else {
            ptz_position raw;
            char error[1024] = {};
            if (!ptz_get_position(link->session_, &raw, error, sizeof error)) {
                result = failure(link->session_, error);
            } else {
                auto reading = [](const ptz_reading &r) -> std::optional<Reading> {
                    if (!r.available) return std::nullopt;
                    return Reading{r.value, field(r.text)};
                };
                snapshot.pan = reading(raw.pan);
                snapshot.tilt = reading(raw.tilt);
                snapshot.zoom = reading(raw.zoom);
                snapshot.move_status = field(raw.move_status);
                ptz_view_scale scale;
                if (ptz_view_scale_at(link->session_, &raw, &scale)) snapshot.view_scale = ViewScale{scale.pan_per_x, scale.tilt_per_y};
                result.ok = true;
            }
        }
        return [done, result, snapshot] { done(result, snapshot); };
    });
}

void link_move(CameraLink *link, std::optional<double> pan, std::optional<double> tilt, std::optional<double> zoom,
               std::function<void(LinkResult)> done) {
    link->run([=]() -> std::function<void()> {
        LinkResult result;
        if (!link->session_) {
            result.error = L"Not connected";
        } else {
            ptz_target t{};
            if (pan) { t.has_pan = 1; t.pan = *pan; }
            if (tilt) { t.has_tilt = 1; t.tilt = *tilt; }
            if (zoom) { t.has_zoom = 1; t.zoom = *zoom; }
            char error[1024] = {};
            if (ptz_move(link->session_, &t, error, sizeof error)) result.ok = true;
            else result = failure(link->session_, error);
        }
        return [done, result] { done(result); };
    });
}

void link_stop(CameraLink *link, std::function<void(LinkResult)> done) {
    link->run([=]() -> std::function<void()> {
        LinkResult result;
        if (!link->session_) {
            result.error = L"Not connected";
        } else {
            char error[1024] = {};
            if (ptz_stop(link->session_, error, sizeof error)) result.ok = true;
            else result = failure(link->session_, error);
        }
        return [done, result] { done(result); };
    });
}

void link_picture(CameraLink *link, std::function<void(LinkResult, std::string)> done) {
    link->run([=]() -> std::function<void()> {
        LinkResult result;
        std::string bytes;
        if (!link->session_) {
            result.error = L"Not connected";
        } else {
            unsigned char *data = nullptr;
            size_t size = 0;
            int width = 0, height = 0;
            char error[1024] = {};
            if (ptz_picture(link->session_, &data, &size, &width, &height, error, sizeof error) && data) {
                bytes.assign(reinterpret_cast<const char *>(data), size);
                ptz_free(data);
                result.ok = true;
            } else {
                result = failure(link->session_, error);
            }
        }
        return [done, result, bytes] { done(result, bytes); };
    });
}

// A JPEG or PNG picture as width x height grey pixels (GDI+ decodes and scales it): the host's part of measuring.
static int grey_picture(void *, const unsigned char *data, size_t size, unsigned char *out, int width, int height) {
    IStream *stream = SHCreateMemStream(data, static_cast<UINT>(size));
    if (!stream) return 0;
    int ok = 0;
    {
        Gdiplus::Bitmap picture(stream);
        Gdiplus::Bitmap small(width, height, PixelFormat32bppARGB);
        if (picture.GetLastStatus() == Gdiplus::Ok && small.GetLastStatus() == Gdiplus::Ok) {
            {
                Gdiplus::Graphics g(&small);
                g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
                g.DrawImage(&picture, 0, 0, width, height);
            }
            Gdiplus::BitmapData pixels;
            Gdiplus::Rect all(0, 0, width, height);
            if (small.LockBits(&all, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &pixels) == Gdiplus::Ok) {
                for (int y = 0; y < height; ++y) {
                    auto *row = static_cast<const BYTE *>(pixels.Scan0) + static_cast<size_t>(y) * pixels.Stride;
                    for (int x = 0; x < width; ++x) {  // BGRA in memory
                        const BYTE *p = row + 4 * x;
                        out[static_cast<size_t>(y) * width + x] = static_cast<unsigned char>((114 * p[0] + 587 * p[1] + 299 * p[2]) / 1000);
                    }
                }
                small.UnlockBits(&pixels);
                ok = 1;
            }
        }
    }
    stream->Release();
    return ok;
}

void link_measure_view(CameraLink *link, std::function<void(LinkResult, ViewScale)> done) {
    link->run([=]() -> std::function<void()> {
        LinkResult result;
        ViewScale view;
        if (!link->session_) {
            result.error = L"Not connected";
        } else {
            ptz_view_scale scale;
            char error[1024] = {};
            if (ptz_measure_view(link->session_, grey_picture, nullptr, &scale, error, sizeof error)) {
                view = ViewScale{scale.pan_per_x, scale.tilt_per_y};
                result.ok = true;
            } else {
                result = failure(link->session_, error);
            }
        }
        return [done, result, view] { done(result, view); };
    });
}

void link_move_in_view(CameraLink *link, double x, double y, std::function<void(LinkResult)> done) {
    link->run([=]() -> std::function<void()> {
        LinkResult result;
        if (!link->session_) {
            result.error = L"Not connected";
        } else {
            char error[1024] = {};
            if (ptz_move_in_view(link->session_, x, y, error, sizeof error)) result.ok = true;
            else result = failure(link->session_, error);
        }
        return [done, result] { done(result); };
    });
}

void link_web_url(CameraLink *link, std::function<void(LinkResult, std::wstring, std::wstring)> done) {
    link->run([=]() -> std::function<void()> {
        LinkResult result;
        std::wstring url, notice;
        if (!link->session_) {
            result.error = L"Not connected";
        } else {
            char u[2048] = {}, n[512] = {}, error[1024] = {};
            if (ptz_web_url(link->session_, u, sizeof u, n, sizeof n, error, sizeof error)) {
                url = W(u);
                notice = W(n);
                if (url.rfind(L"http://", 0) == 0 || url.rfind(L"https://", 0) == 0) result.ok = true;
                else result.error = L"Camera web interface address is invalid";
            } else {
                result = failure(link->session_, error);
            }
        }
        return [done, result, url, notice] { done(result, url, notice); };
    });
}

void link_disconnect(CameraLink *link, std::function<void()> done) {
    link->run([=]() -> std::function<void()> {
        if (link->session_) {
            ptz_close(link->session_);
            link->session_ = nullptr;
        }
        return done;
    });
}
