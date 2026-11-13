// Internal C++ interfaces of the shared core. Hosts use only ptzcore.h.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ptz {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Range {
    bool available = false;
    double min = 0, max = 0;
};

struct Reading {
    bool available = false;
    double value = 0;
    std::string text;
};

struct Position {
    Reading pan, tilt, zoom;
    std::string move_status;
};

struct Target {
    std::optional<double> pan, tilt, zoom;
};

struct Hooks {
    std::function<void(const std::string &)> log;
    std::function<std::optional<std::string>(const std::string &message, bool secret)> prompt;
    std::function<bool(const std::string &title, const std::string &message)> confirm;
    const volatile int *cancel = nullptr;
    bool cancelled() const { return cancel && *cancel; }
};

// ---- numbers --------------------------------------------------------------
std::optional<double> parse_number(std::string_view text);  // locale independent, finite only
std::string format_number(double value);                     // shortest round-trip form

// ---- text -----------------------------------------------------------------
std::string lower(std::string_view text);
std::string trim(std::string_view text);
std::string printable(std::string_view text);  // strips control characters
std::string base64_encode(const unsigned char *data, size_t size);
void sha1(const unsigned char *data, size_t size, unsigned char digest[20]);
std::string random_bytes(size_t count);
std::string bracket_host(const std::string &host);  // IPv6 literal -> [addr]
std::string validate_host(std::string host);         // throws Error

#if defined(_WIN32)
std::wstring widen(const std::string &text);   // UTF-8 -> UTF-16
std::string narrow(const std::wstring &text);  // UTF-16 -> UTF-8
std::string windows_error(unsigned long code);
#endif

// ---- XML ------------------------------------------------------------------
struct XmlNode {
    std::string name;  // local name, namespace prefix removed
    std::map<std::string, std::string> attributes;  // local names
    std::string text;
    std::vector<XmlNode> children;

    const XmlNode *child(std::string_view local) const;
    std::vector<const XmlNode *> children_named(std::string_view local) const;
    const XmlNode *find(std::string_view local) const;  // depth-first, includes self
    std::string child_text(std::string_view local) const;
    std::string attribute(std::string_view local) const;
};
XmlNode xml_parse(const std::string &document);
std::string xml_escape(std::string_view text);

// ---- HTTP -----------------------------------------------------------------
struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::string body;
    std::vector<std::string> headers;
    std::string user, password;
    bool digest_auth = false;
    double timeout = 15;
    size_t max_body = 4 * 1024 * 1024;  // larger bodies are truncated, not an error
};

struct HttpResponse {
    bool transport_ok = false;
    bool tls_verify_failed = false;
    std::string transport_error;
    long status = 0;
    std::string body;
    std::map<std::string, std::string> headers;  // lower-case names
};

HttpResponse http_perform(const HttpRequest &request);
void http_global_init();

// ---- camera backends ------------------------------------------------------
struct CameraParams {
    std::string host;  // address actually dialled (127.0.0.1 when tunnelled)
    int port = 80;
    std::string user, password;
    int profile = 0;
    double timeout = 15;
    bool tunneled = false;
};

class Camera {
public:
    virtual ~Camera() = default;
    virtual std::string name() const = 0;
    virtual std::string units() const = 0;
    virtual bool zoom_integer() const { return false; }
    Range pan_range, tilt_range, zoom_range;
    virtual Position position() = 0;
    virtual void move(const Target &target) = 0;
    virtual void stop() = 0;
    // A still picture of the current view, JPEG or PNG bytes.
    virtual std::string picture() = 0;
    // Whether move_in_view() works on this camera.
    virtual bool view_moves() const = 0;
    // Brings the point (x, y) of the current picture to its centre: x and y run from -1 at the picture's left and
    // bottom edges to 1 at its right and top edges. width and height are the picture's size in pixels.
    virtual void move_in_view(double x, double y, int width, int height) = 0;
};

// ---- pictures (picture.cpp) -----------------------------------------------
std::optional<std::pair<int, int>> picture_size(const std::string &data);  // JPEG or PNG: width, height
std::string fetch_picture(const std::string &url, const std::string &user, const std::string &password, double timeout);

std::unique_ptr<Camera> make_onvif(const CameraParams &params, const Hooks &hooks);
std::unique_ptr<Camera> make_axis(const CameraParams &params, const Hooks &hooks);

// ---- SSH tunnel -----------------------------------------------------------
struct TunnelParams {
    std::string ssh_host, ssh_user, ssh_password;
    int ssh_port = 22;
    int https_port = 443;  // Windows pre-forwards it: no multiplexing for later forwards
    std::string ssh_identity;
    std::string camera_host;
    int camera_port = 80;
    std::string askpass_path;
    double timeout = 15;
};

class Tunnel {
public:
    virtual ~Tunnel() = default;
    virtual int local_port() const = 0;
    virtual bool alive() = 0;
    virtual int forward(int camera_port) = 0;  // adds another -L, returns local port
};

std::unique_ptr<Tunnel> open_tunnel(const TunnelParams &params, const Hooks &hooks);

// Shared SSH log handling (tunnel_common.cpp).
std::vector<std::string> log_lines(const std::string &text);
std::string ssh_failure_message(const std::string &log);
// True when ssh died on a packet integrity check (some client/server cipher pairings, notably
// Windows OpenSSH, fail this way); the tunnels then retry once with ssh_fallback_options().
bool ssh_integrity_failure(const std::string &log);
std::vector<std::string> ssh_fallback_options();
bool replace_changed_host_key(const std::string &log, const std::string &host, int port, const Hooks &hooks,
                              const std::function<std::string()> &find_keygen,
                              const std::function<int(const std::vector<std::string> &)> &run);

// ---- camera web interface -------------------------------------------------
struct WebTarget {
    std::string camera_host;  // as the operator entered it
    int camera_port = 80;
    int local_http_port = 80;
    int https_port = 443;
    double timeout = 15;
    Tunnel *tunnel = nullptr;
};

struct WebResult {
    std::string url, notice;
};

class WebInterface {
public:
    explicit WebInterface(WebTarget target) : target_(std::move(target)) {}
    WebResult locate();

private:
    std::string address(const std::string &scheme, int port) const;
    struct Probe {
        bool reachable = false;
        std::optional<int> https_redirect_port;
        std::string final_url;
        std::string notice;
    };
    Probe probe(const std::string &url);
    WebTarget target_;
    std::optional<int> https_local_port_, https_camera_port_;
};

}  // namespace ptz
