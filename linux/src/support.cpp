// Text helpers, themes, settings and the camera-link worker.
#include "app.h"

#include <QCoreApplication>
#include <QDir>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QImage>
#include <QPalette>
#include <QProcess>
#include <QSettings>
#include <QStyleHints>

#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

// ---- text --------------------------------------------------------------------

QString format_value(double value) {
    char buffer[64];
    if (value == static_cast<double>(static_cast<long long>(value)) && value > -1e15 && value < 1e15) {
        std::snprintf(buffer, sizeof buffer, "%lld", static_cast<long long>(value));
        return QString::fromLatin1(buffer);
    }
    // Qt never calls setlocale for LC_NUMERIC on our behalf, so "%f" always uses '.'.
    std::snprintf(buffer, sizeof buffer, "%.4f", value);
    std::string text = buffer;
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    return QString::fromStdString(text);
}

bool parse_value(const QString &text, double &value) {
    return ptz_parse_number(text.toUtf8().constData(), &value) != 0;
}

QFont mono_font(double pixels, bool bold) {
    // JetBrains Mono ships inside the Flatpak; "monospace" is the fontconfig fallback.
    static const QString family = [] {
        return QFontDatabase::families().contains("JetBrains Mono") ? QString("JetBrains Mono") : QString("monospace");
    }();
    QFont font(family);
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(std::max(1, static_cast<int>(pixels + 0.5)));
    font.setBold(bold);
    return font;
}

bool Snapshot::operator==(const Snapshot &o) const {
    auto same = [](const std::optional<Reading> &a, const std::optional<Reading> &b) {
        return a.has_value() == b.has_value() && (!a || a->text == b->text);
    };
    return same(pan, o.pan) && same(tilt, o.tilt) && same(zoom, o.zoom) && move_status == o.move_status;
}

// ---- theme -------------------------------------------------------------------

QString phosphor_title(Phosphor p) {
    switch (p) {
        case Phosphor::System: return "System";
        case Phosphor::Green: return "Green Phosphor";
        case Phosphor::Amber: return "Amber Phosphor";
        case Phosphor::Blue: return "Blue Phosphor";
        case Phosphor::Mono: return "Mono";
    }
    return "System";
}

QColor blend(const QColor &a, const QColor &b, double t) {
    auto mix = [t](int x, int y) { return static_cast<int>(x + (y - x) * t + 0.5); };
    return QColor(mix(a.red(), b.red()), mix(a.green(), b.green()), mix(a.blue(), b.blue()));
}

// The desktop's palette as Qt found it at startup, before any phosphor palette is applied.
static QPalette &desktop_palette() {
    static QPalette palette = QGuiApplication::palette();
    return palette;
}

static std::optional<QColor> kde_color(const QString &key) {
    QSettings kde(QDir::homePath() + "/.config/kdeglobals", QSettings::IniFormat);
    QStringList rgb = kde.value(key).toStringList();
    if (rgb.size() != 3) return std::nullopt;
    return QColor(rgb[0].toInt(), rgb[1].toInt(), rgb[2].toInt());
}

// The desktop's light or dark preference: Qt reads it from the desktop portal (the Flatpak's way in);
// otherwise ask the desktop directly.
static bool desktop_prefers_dark() {
    switch (QGuiApplication::styleHints()->colorScheme()) {
        case Qt::ColorScheme::Dark: return true;
        case Qt::ColorScheme::Light: return false;
        default: break;
    }
    QByteArray gtk = qgetenv("GTK_THEME").toLower();
    if (!gtk.isEmpty()) return gtk.contains("dark");
    QProcess gsettings;
    gsettings.start("gsettings", {"get", "org.gnome.desktop.interface", "color-scheme"});
    if (gsettings.waitForFinished(1500) && gsettings.exitCode() == 0)
        if (gsettings.readAllStandardOutput().contains("prefer-dark")) return true;
    gsettings.start("gsettings", {"get", "org.gnome.desktop.interface", "gtk-theme"});
    if (gsettings.waitForFinished(1500) && gsettings.exitCode() == 0)
        if (gsettings.readAllStandardOutput().toLower().contains("dark")) return true;
    if (auto window = kde_color("Colors:Window/BackgroundNormal")) return window->lightness() < 128;
    return desktop_palette().color(QPalette::Window).lightness() < 128;
}

// KDE's selection colour when set; otherwise the GNOME (Adwaita) accent blue.
static QColor desktop_accent() {
    if (auto selection = kde_color("Colors:Selection/BackgroundNormal")) return *selection;
    return QColor(0x35, 0x84, 0xE4);
}

Theme make_theme(Phosphor p) {
    Theme t{};
    if (p == Phosphor::System) {
        static const QColor accent = desktop_accent();
        static const bool dark = desktop_prefers_dark();
        t.dark = dark;
        if (t.dark) {
            t.background = QColor(32, 32, 32);
            t.panel = QColor(43, 43, 43);
            t.border = QColor(70, 70, 70);
            t.text = QColor(240, 240, 240);
            t.secondary = QColor(160, 160, 160);
            t.field = QColor(30, 30, 30);
            t.button = QColor(60, 60, 60);
        } else {
            t.background = QColor(243, 243, 243);
            t.panel = QColor(255, 255, 255);
            t.border = QColor(214, 214, 214);
            t.text = QColor(26, 26, 26);
            t.secondary = QColor(100, 100, 100);
            t.field = QColor(250, 250, 250);
            t.button = QColor(232, 232, 232);
        }
        t.primary = accent;
        t.accent = accent;
        t.accent_text = accent.lightness() < 160 ? QColor(255, 255, 255) : QColor(0, 0, 0);
        t.grid = blend(t.panel, t.secondary, 0.18);
        t.axis = blend(t.panel, t.secondary, 0.55);
        t.start = QColor(230, 140, 30);
        t.target = QColor(226, 60, 120);
        t.warning = t.dark ? QColor(255, 99, 90) : QColor(196, 43, 28);
        t.good = t.dark ? QColor(108, 203, 95) : QColor(16, 124, 16);
        t.field_text = t.text;
        t.button_text = t.text;
        return t;
    }
    QColor ph = p == Phosphor::Green ? QColor(0x3D, 0xFF, 0x86)
              : p == Phosphor::Amber ? QColor(0xFF, 0xB3, 0x3D)
              : p == Phosphor::Blue  ? QColor(0x5C, 0xC8, 0xFF)
                                     : QColor(0xE6, 0xE6, 0xE6);
    t.dark = true;
    t.background = QColor(7, 9, 10);
    t.panel = QColor(13, 17, 18);
    t.border = blend(t.panel, ph, 0.28);
    t.grid = blend(t.panel, ph, 0.12);
    t.axis = blend(t.panel, ph, 0.45);
    t.primary = ph;
    t.secondary = blend(t.panel, ph, 0.6);
    t.text = blend(ph, QColor(255, 255, 255), 0.55);
    t.start = blend(t.panel, ph, 0.7);
    t.target = blend(ph, QColor(255, 255, 255), 0.5);
    t.warning = QColor(0xFF, 0x5A, 0x4F);
    t.good = ph;
    t.field = QColor(4, 6, 7);
    t.field_text = t.text;
    t.button = blend(t.panel, ph, 0.14);
    t.button_text = t.text;
    t.accent = blend(t.panel, ph, 0.55);
    t.accent_text = t.background;
    return t;
}

// ---- settings ----------------------------------------------------------------

// ~/.config/PTZControl/PTZControl.conf (or under $XDG_CONFIG_HOME).
static QSettings store() { return QSettings("PTZControl", "PTZControl"); }

Settings Settings::load() {
    desktop_palette();  // capture before any theme replaces the application palette
    Settings s;
    if (const char *user = std::getenv("PTZ_USER")) s.camera_user = QString::fromUtf8(user);
    QSettings q = store();
    s.route = q.value("Route", s.route).toInt() ? 1 : 0;
    s.vendor = q.value("Vendor", s.vendor).toInt() ? 1 : 0;
    s.camera_host = q.value("CameraHost", s.camera_host).toString();
    s.camera_port = q.value("CameraPort", s.camera_port).toInt();
    s.camera_user = q.value("CameraUser", s.camera_user).toString();
    s.https_port = q.value("HttpsPort", s.https_port).toInt();
    s.profile = q.value("Profile", s.profile).toInt();
    double timeout;
    if (parse_value(q.value("Timeout").toString(), timeout) && timeout > 0) s.timeout = timeout;
    s.ssh_host = q.value("SshHost", s.ssh_host).toString();
    s.ssh_user = q.value("SshUser", s.ssh_user).toString();
    s.ssh_identity = q.value("SshIdentity", s.ssh_identity).toString();
    s.ssh_port = q.value("SshPort", s.ssh_port).toInt();
    s.theme = q.value("Theme", s.theme).toInt();
    if (s.theme < 0 || s.theme > 4) s.theme = 0;
    s.show_grid = q.value("ShowGrid", s.show_grid).toBool();
    return s;
}

void Settings::save() const {
    QSettings q = store();
    q.setValue("Route", route);
    q.setValue("Vendor", vendor);
    q.setValue("CameraHost", camera_host);
    q.setValue("CameraPort", camera_port);
    q.setValue("CameraUser", camera_user);
    q.setValue("HttpsPort", https_port);
    q.setValue("Profile", profile);
    q.setValue("Timeout", format_value(timeout));
    q.setValue("SshHost", ssh_host);
    q.setValue("SshUser", ssh_user);
    q.setValue("SshIdentity", ssh_identity);
    q.setValue("SshPort", ssh_port);
    q.setValue("Theme", theme);
    q.setValue("ShowGrid", show_grid);
}

// ---- camera link -------------------------------------------------------------

class CameraLink {
public:
    CameraLink() : receiver_(new QObject), thread_([this] { loop(); }) {}

    ~CameraLink() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        ready_.notify_all();
        thread_.join();
        live_stop(live_);
        if (session_) ptz_close(session_);
        delete receiver_;  // drops callbacks still queued for it
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

    void post(std::function<void()> fn) { QMetaObject::invokeMethod(receiver_, std::move(fn), Qt::QueuedConnection); }

    // Core threads: run on the UI thread and wait (modal prompts).
    void sync(std::function<void()> fn) { QMetaObject::invokeMethod(receiver_, std::move(fn), Qt::BlockingQueuedConnection); }

    LinkDelegate delegate;
    ptz_session *session_ = nullptr;  // worker thread only
    LiveVideo *live_ = nullptr;       // worker thread only
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

    QObject *receiver_;  // lives on the UI thread
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> jobs_;
    bool quit_ = false;
    std::thread thread_;
};

CameraLink *create_link() { return new CameraLink; }
void destroy_link(CameraLink *link) { delete link; }
void link_set_delegate(CameraLink *link, LinkDelegate delegate) { link->delegate = std::move(delegate); }
bool link_busy(CameraLink *link) { return link->pending_ > 0; }
void link_cancel_connect(CameraLink *link) { link->cancel_ = 1; }

namespace {

template <size_t N>
QString field(const char (&buffer)[N]) {
    return QString::fromUtf8(buffer, static_cast<int>(strnlen(buffer, N)));
}

void wipe(std::string &s) {
    volatile char *p = s.data();
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
}

void on_log(void *context, const char *line) {
    auto *link = static_cast<CameraLink *>(context);
    QString text = QString::fromUtf8(line);
    link->post([link, text] {
        if (link->delegate.log) link->delegate.log(text);
    });
}

int on_prompt(void *context, const char *message, int secret, char *answer, size_t size) {
    auto *link = static_cast<CameraLink *>(context);
    std::optional<QString> reply;
    QString text = QString::fromUtf8(message);
    link->sync([&] {
        if (link->delegate.prompt) reply = link->delegate.prompt(text, secret != 0);
    });
    if (!reply) return 0;
    std::string utf8 = reply->toStdString();
    reply->fill(QChar(0));
    if (utf8.size() >= size) utf8.resize(size - 1);
    std::memcpy(answer, utf8.c_str(), utf8.size() + 1);
    wipe(utf8);
    return 1;
}

int on_confirm(void *context, const char *title, const char *message) {
    auto *link = static_cast<CameraLink *>(context);
    bool yes = false;
    QString t = QString::fromUtf8(title), m = QString::fromUtf8(message);
    link->sync([&] {
        if (link->delegate.confirm) yes = link->delegate.confirm(t, m);
    });
    return yes ? 1 : 0;
}

LinkResult failure(ptz_session *session, const char *error) {
    LinkResult r;
    r.error = QString::fromUtf8(error);
    r.link_lost = session && !ptz_link_alive(session);
    return r;
}

}  // namespace

void link_connect(CameraLink *link, const Settings &s, const QString &camera_password, const QString &ssh_password,
                  std::function<void(LinkResult, CameraInfo)> done) {
    link->cancel_ = 0;
    // ssh runs this same executable as SSH_ASKPASS (inside the Flatpak: /app/bin/ptz-control).
    std::string askpass = QCoreApplication::applicationFilePath().toStdString();
    std::string cam_pw = camera_password.toStdString(), ssh_pw = ssh_password.toStdString();
    link->run([=]() mutable -> std::function<void()> {
        std::string camera_host = s.camera_host.toStdString(), camera_user = s.camera_user.toStdString();
        std::string ssh_host = s.ssh_host.toStdString(), ssh_user = s.ssh_user.toStdString();
        std::string identity = s.ssh_identity.toStdString();
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
        wipe(cam_pw);
        wipe(ssh_pw);
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
            result.error = "Not connected";
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
            result.error = "Not connected";
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
            result.error = "Not connected";
        } else {
            char error[1024] = {};
            if (ptz_stop(link->session_, error, sizeof error)) result.ok = true;
            else result = failure(link->session_, error);
        }
        return [done, result] { done(result); };
    });
}

void link_picture(CameraLink *link, std::function<void(LinkResult, QImage)> done) {
    link->run([=]() -> std::function<void()> {
        LinkResult result;
        QImage image;
        if (!link->session_) {
            result.error = "Not connected";
        } else {
            unsigned char *data = nullptr;
            size_t size = 0;
            int width = 0, height = 0;
            char error[1024] = {};
            if (!ptz_picture(link->session_, &data, &size, &width, &height, error, sizeof error) || !data) {
                result = failure(link->session_, error);
            } else {
                image = QImage::fromData(data, static_cast<int>(size));
                ptz_free(data);
                if (image.isNull()) result.error = "The camera's picture could not be read";
                else result.ok = true;
            }
        }
        return [done, result, image] { done(result, image); };
    });
}

void link_live_start(CameraLink *link, std::function<void(QImage)> picture, std::function<void(QString)> ended,
                     std::function<void(LinkResult, QString)> done) {
    link->run([=]() -> std::function<void()> {
        LinkResult result;
        QString description;
        if (!link->session_) {
            result.error = "Not connected";
        } else {
            live_stop(link->live_);
            link->live_ = nullptr;
            QString error;
            LivePost post = [link](std::function<void()> fn) { link->post(std::move(fn)); };
            link->live_ = live_start(link->session_, post, picture, ended, &description, &error);
            if (link->live_) {
                result.ok = true;
            } else {
                result.error = error;
                result.link_lost = !ptz_link_alive(link->session_);
            }
        }
        return [done, result, description] { done(result, description); };
    });
}

void link_live_stop(CameraLink *link, std::function<void()> done) {
    link->run([=]() -> std::function<void()> {
        live_stop(link->live_);
        link->live_ = nullptr;
        return [done] { if (done) done(); };
    });
}

void link_web_url(CameraLink *link, std::function<void(LinkResult, QString, QString)> done) {
    link->run([=]() -> std::function<void()> {
        LinkResult result;
        QString url, notice;
        if (!link->session_) {
            result.error = "Not connected";
        } else {
            char u[2048] = {}, n[512] = {}, error[1024] = {};
            if (ptz_web_url(link->session_, u, sizeof u, n, sizeof n, error, sizeof error)) {
                url = QString::fromUtf8(u);
                notice = QString::fromUtf8(n);
                if (url.startsWith("http://") || url.startsWith("https://")) result.ok = true;
                else result.error = "Camera web interface address is invalid";
            } else {
                result = failure(link->session_, error);
            }
        }
        return [done, result, url, notice] { done(result, url, notice); };
    });
}

void link_disconnect(CameraLink *link, std::function<void()> done) {
    link->run([=]() -> std::function<void()> {
        live_stop(link->live_);
        link->live_ = nullptr;
        if (link->session_) {
            ptz_close(link->session_);
            link->session_ = nullptr;
        }
        return done;
    });
}
