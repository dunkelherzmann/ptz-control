// C ABI over the C++ core. Every error crossing it is redacted.
#include "ptzcore.h"
#include "internal.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

struct ptz_session {
    ptz::Hooks hooks;
    std::string camera_password, ssh_password;
    std::unique_ptr<ptz::Tunnel> tunnel;
    std::unique_ptr<ptz::Camera> camera;
    std::unique_ptr<ptz::WebInterface> web;
    int picture_width = 0, picture_height = 0;  // the last picture's size, for aiming in it

    std::string redact(std::string message) const {
        for (const std::string *secret : {&camera_password, &ssh_password}) {
            if (secret->empty()) continue;
            for (size_t at; (at = message.find(*secret)) != std::string::npos;)
                message.replace(at, secret->size(), "[redacted]");
        }
        return ptz::printable(message);
    }
};

namespace {

void copy(char *out, size_t size, const std::string &text) {
    if (!out || !size) return;
    std::strncpy(out, text.c_str(), size - 1);
    out[size - 1] = '\0';
}

std::string string_or_empty(const char *text) { return text ? text : ""; }

void export_reading(const ptz::Reading &in, ptz_reading &out) {
    out.available = in.available;
    out.value = in.value;
    copy(out.text, sizeof out.text, in.text);
}

ptz_range export_range(const ptz::Range &range) { return {range.available, range.min, range.max}; }

template <typename F>
int guarded(ptz_session *session, char *error, size_t error_size, F &&body) {
    try {
        body();
        return 1;
    } catch (const std::exception &e) {
        copy(error, error_size, session ? session->redact(e.what()) : e.what());
    } catch (...) {
        copy(error, error_size, "Unexpected error");
    }
    return 0;
}

}  // namespace

extern "C" ptz_session *ptz_connect(const ptz_connect_params *params, const ptz_callbacks *callbacks,
                                    volatile int *cancel, char *error, size_t error_size) {
    auto session = std::make_unique<ptz_session>();
    session->camera_password = string_or_empty(params->camera_password);
    session->ssh_password = params->route == PTZ_ROUTE_SSH ? string_or_empty(params->ssh_password) : "";
    session->hooks.cancel = cancel;
    if (callbacks) {
        ptz_callbacks cb = *callbacks;
        ptz_session *raw = session.get();
        if (cb.log)
            session->hooks.log = [cb, raw](const std::string &line) { cb.log(cb.context, raw->redact(line).c_str()); };
        else
            session->hooks.log = [](const std::string &) {};
        if (cb.prompt)
            session->hooks.prompt = [cb](const std::string &message, bool secret) -> std::optional<std::string> {
                std::string answer(1025, '\0');
                if (!cb.prompt(cb.context, message.c_str(), secret, answer.data(), answer.size())) return std::nullopt;
                answer.resize(std::strlen(answer.c_str()));
                return answer;
            };
        if (cb.confirm)
            session->hooks.confirm = [cb](const std::string &title, const std::string &message) {
                return cb.confirm(cb.context, title.c_str(), message.c_str()) != 0;
            };
    } else {
        session->hooks.log = [](const std::string &) {};
    }

    int ok = guarded(session.get(), error, error_size, [&] {
        auto port_ok = [](int port) { return port >= 1 && port <= 65535; };
        std::string camera_host = ptz::validate_host(string_or_empty(params->camera_host));
        std::string camera_user = ptz::trim(string_or_empty(params->camera_user));
        if (camera_user.empty()) throw ptz::Error("Camera Username is required");
        if (!port_ok(params->camera_port) || !port_ok(params->https_port))
            throw ptz::Error("Ports must be integers between 1 and 65535");
        if (params->profile < 0) throw ptz::Error("Profile index must be nonnegative");
        double timeout = params->timeout > 0 ? params->timeout : 15;

        ptz::CameraParams camera{camera_host, params->camera_port, camera_user, session->camera_password,
                                 params->profile, timeout, false};
        if (params->route == PTZ_ROUTE_SSH) {
            ptz::TunnelParams tunnel;
            tunnel.ssh_host = ptz::validate_host(string_or_empty(params->ssh_host));
            tunnel.ssh_user = ptz::trim(string_or_empty(params->ssh_user));
            if (tunnel.ssh_user.empty()) throw ptz::Error("SSH Username is required");
            if (tunnel.ssh_user.front() == '-') throw ptz::Error("SSH Username cannot start with '-'");
            if (!port_ok(params->ssh_port)) throw ptz::Error("Ports must be integers between 1 and 65535");
            for (char c : session->ssh_password)
                if (c == '\n' || c == '\r') throw ptz::Error("Host Password must be a single line");
            tunnel.ssh_password = session->ssh_password;
            tunnel.ssh_port = params->ssh_port;
            tunnel.https_port = params->https_port;
            tunnel.ssh_identity = ptz::trim(string_or_empty(params->ssh_identity));
            tunnel.camera_host = camera_host;
            tunnel.camera_port = params->camera_port;
            tunnel.askpass_path = string_or_empty(params->askpass_path);
            tunnel.timeout = timeout;
            session->tunnel = ptz::open_tunnel(tunnel, session->hooks);
            camera.host = "127.0.0.1";
            camera.port = session->tunnel->local_port();
            camera.tunneled = true;
        }
        session->hooks.log("Connecting to camera " + camera_host + " (" +
                           (params->vendor == PTZ_VENDOR_AXIS ? "Axis VAPIX" : "ONVIF") + "); no movement is sent");
        session->camera = params->vendor == PTZ_VENDOR_AXIS ? ptz::make_axis(camera, session->hooks)
                                                            : ptz::make_onvif(camera, session->hooks);
        ptz::WebTarget web{camera_host, params->camera_port, camera.port, params->https_port, timeout,
                           session->tunnel.get()};
        session->web = std::make_unique<ptz::WebInterface>(web);
    });
    if (!ok) return nullptr;
    return session.release();
}

extern "C" void ptz_info_get(ptz_session *session, ptz_info *info) {
    std::memset(info, 0, sizeof *info);
    copy(info->name, sizeof info->name, session->camera->name());
    copy(info->units, sizeof info->units, session->camera->units());
    info->pan = export_range(session->camera->pan_range);
    info->tilt = export_range(session->camera->tilt_range);
    info->zoom = export_range(session->camera->zoom_range);
    info->zoom_integer = session->camera->zoom_integer();
    info->view_moves = session->camera->view_moves();
}

extern "C" int ptz_picture(ptz_session *session, unsigned char **data, size_t *size, int *width, int *height,
                           char *error, size_t error_size) {
    *data = nullptr;
    *size = 0;
    *width = *height = 0;
    return guarded(session, error, error_size, [&] {
        if (!ptz_link_alive(session)) throw ptz::Error("SSH connection closed; reconnect to continue");
        std::string bytes = session->camera->picture();
        auto dimensions = ptz::picture_size(bytes);
        auto *copy = static_cast<unsigned char *>(std::malloc(bytes.size()));
        if (!copy) throw ptz::Error("Not enough memory for the camera's picture");
        std::memcpy(copy, bytes.data(), bytes.size());
        *data = copy;
        *size = bytes.size();
        *width = session->picture_width = dimensions->first;
        *height = session->picture_height = dimensions->second;
    });
}

extern "C" void ptz_free(void *data) { std::free(data); }

extern "C" int ptz_move_in_view(ptz_session *session, double x, double y, char *error, size_t error_size) {
    return guarded(session, error, error_size, [&] {
        if (!ptz_link_alive(session)) throw ptz::Error("SSH connection closed; the move was not sent");
        if (!std::isfinite(x) || !std::isfinite(y) || x < -1 || x > 1 || y < -1 || y > 1)
            throw ptz::Error("The point to aim at must lie inside the picture");
        session->camera->move_in_view(x, y, session->picture_width, session->picture_height);
    });
}

extern "C" int ptz_get_position(ptz_session *session, ptz_position *position, char *error, size_t error_size) {
    std::memset(position, 0, sizeof *position);
    return guarded(session, error, error_size, [&] {
        if (!ptz_link_alive(session)) throw ptz::Error("SSH connection closed; reconnect to continue");
        auto p = session->camera->position();
        export_reading(p.pan, position->pan);
        export_reading(p.tilt, position->tilt);
        export_reading(p.zoom, position->zoom);
        copy(position->move_status, sizeof position->move_status, p.move_status);
    });
}

extern "C" int ptz_move(ptz_session *session, const ptz_target *target, char *error, size_t error_size) {
    return guarded(session, error, error_size, [&] {
        if (!ptz_link_alive(session)) throw ptz::Error("SSH connection closed; coordinates were not sent");
        ptz::Target t;
        if (target->has_pan) t.pan = target->pan;
        if (target->has_tilt) t.tilt = target->tilt;
        if (target->has_zoom) t.zoom = target->zoom;
        session->camera->move(t);
    });
}

extern "C" int ptz_stop(ptz_session *session, char *error, size_t error_size) {
    return guarded(session, error, error_size, [&] {
        if (!ptz_link_alive(session)) throw ptz::Error("SSH connection closed; stop was not sent");
        session->camera->stop();
    });
}

extern "C" int ptz_web_url(ptz_session *session, char *url, size_t url_size, char *notice, size_t notice_size,
                           char *error, size_t error_size) {
    copy(url, url_size, "");
    copy(notice, notice_size, "");
    return guarded(session, error, error_size, [&] {
        auto result = session->web->locate();
        copy(url, url_size, result.url);
        copy(notice, notice_size, result.notice);
    });
}

extern "C" int ptz_link_alive(ptz_session *session) { return !session->tunnel || session->tunnel->alive(); }

extern "C" void ptz_close(ptz_session *session) {
    if (!session) return;
    session->web.reset();
    session->camera.reset();
    session->tunnel.reset();
    std::fill(session->camera_password.begin(), session->camera_password.end(), '\0');
    std::fill(session->ssh_password.begin(), session->ssh_password.end(), '\0');
    delete session;
}

extern "C" int ptz_parse_number(const char *text, double *value) {
    auto parsed = ptz::parse_number(text ? text : "");
    if (!parsed) return 0;
    *value = *parsed;
    return 1;
}

// The app's version, written by scripts/version.sh before each build.
#if __has_include("ptz_version.h")
#include "ptz_version.h"
#else
#define PTZ_VERSION "0.0.0"
#endif

extern "C" const char *ptz_core_version(void) { return PTZ_VERSION; }
