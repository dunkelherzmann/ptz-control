// Axis VAPIX ptz.cgi with HTTP Digest, camera channel 1.
#include "internal.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace ptz {

namespace {

std::string url_encode(const std::string &text) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out.push_back(static_cast<char>(c));
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

class Axis final : public Camera {
public:
    Axis(CameraParams params) : p_(std::move(params)) {
        url_ = "http://" + bracket_host(p_.host) + ":" + std::to_string(p_.port) + "/axis-cgi/com/ptz.cgi";
        auto limits = query("limits");
        auto load = [&](const char *axis, Range &range) {
            auto low = limits.find(std::string("min") + axis), high = limits.find(std::string("max") + axis);
            if (low == limits.end() || high == limits.end()) return;
            auto l = parse_number(low->second), h = parse_number(high->second);
            if (!l || !h) throw Error(std::string("Axis returned non-numeric ") + axis + " limits");
            if (*l > *h) throw Error(std::string("Axis returned invalid ") + axis + " limits");
            range = {true, *l, *h};
        };
        load("pan", pan_range);
        load("tilt", tilt_range);
        load("zoom", zoom_range);
        if (!pan_range.available && !tilt_range.available && !zoom_range.available)
            throw Error("Axis did not report usable PTZ limits for camera channel 1");
    }

    std::string name() const override { return "Axis · VAPIX / HTTP Digest (channel 1)"; }
    std::string units() const override {
        return "Axis native coordinates: pan/tilt in degrees; zoom is an integer, not optical magnification";
    }
    bool zoom_integer() const override { return true; }

    Position position() override {
        auto reported = query("position");
        Position result;
        auto load = [&](const char *axis, Reading &reading) {
            auto it = reported.find(axis);
            if (it == reported.end()) return;
            if (auto value = parse_number(it->second)) reading = {true, *value, it->second};
        };
        load("pan", result.pan);
        load("tilt", result.tilt);
        load("zoom", result.zoom);
        if (!result.pan.available && !result.tilt.available && !result.zoom.available)
            throw Error("Axis did not report its current coordinates");
        return result;
    }

    void move(const Target &target) override {
        std::vector<std::pair<std::string, std::string>> parameters;
        auto add = [&](const char *axis, const std::optional<double> &value, const Range &range) {
            if (!value) return;
            if (!range.available) throw Error(std::string("Axis did not advertise ") + axis + " limits; move not sent");
            if (*value < range.min || *value > range.max)
                throw Error(std::string(axis) + " " + format_number(*value) + " is outside the camera's range [" +
                            format_number(range.min) + ", " + format_number(range.max) + "]");
            if (std::string(axis) == "zoom") {
                if (std::floor(*value) != *value) throw Error("Axis zoom must be an integer in the advertised range");
                parameters.emplace_back(axis, std::to_string(static_cast<long long>(*value)));
            } else {
                parameters.emplace_back(axis, format_number(*value));
            }
        };
        add("pan", target.pan, pan_range);
        add("tilt", target.tilt, tilt_range);
        add("zoom", target.zoom, zoom_range);
        // Omitted axes are not sent, so VAPIX leaves them where they are.
        if (parameters.empty()) throw Error("All axes unchanged; no move sent");
        request(parameters);
    }

    void stop() override { request({{"move", "stop"}}); }

    std::string picture() override {
        return fetch_picture("http://" + bracket_host(p_.host) + ":" + std::to_string(p_.port) +
                                 "/axis-cgi/jpg/image.cgi?camera=1",
                             p_.user, p_.password, p_.timeout);
    }

    bool view_moves() const override { return true; }

    void move_in_view(double x, double y, int width, int height) override {
        if (width <= 0 || height <= 0) throw Error("Take a picture before aiming in it");
        // VAPIX center= takes pixel coordinates in the picture, from its top-left corner.
        long px = std::lround((x + 1) / 2 * width), py = std::lround((1 - y) / 2 * height);
        px = std::clamp(px, 0L, static_cast<long>(width) - 1);
        py = std::clamp(py, 0L, static_cast<long>(height) - 1);
        request({{"center", std::to_string(px) + "," + std::to_string(py)},
                 {"imagewidth", std::to_string(width)},
                 {"imageheight", std::to_string(height)}});
    }

private:
    std::map<std::string, std::string> query(const std::string &kind) {
        std::map<std::string, std::string> values;
        std::istringstream lines(request({{"query", kind}}));
        for (std::string line; std::getline(lines, line);) {
            auto eq = line.find('=');
            if (eq != std::string::npos) values[lower(trim(line.substr(0, eq)))] = trim(line.substr(eq + 1));
        }
        return values;
    }

    std::string request(const std::vector<std::pair<std::string, std::string>> &parameters) {
        std::string url = url_ + "?camera=1";
        for (const auto &[key, value] : parameters) url += "&" + url_encode(key) + "=" + url_encode(value);
        HttpRequest http;
        http.url = url;
        http.timeout = p_.timeout;
        http.digest_auth = true;
        http.user = p_.user;
        http.password = p_.password;
        HttpResponse response = http_perform(http);
        if (!response.transport_ok) throw Error("Camera unreachable: " + response.transport_error);
        if (response.status >= 300 && response.status < 400)
            throw Error("Axis redirected the request; use an HTTP camera endpoint (HTTPS is not supported)");
        if (response.status == 401) throw Error("Axis rejected the credentials (HTTP 401)");
        if (response.status >= 400) throw Error("Axis returned HTTP " + std::to_string(response.status));
        std::string text = trim(response.body);
        std::istringstream lines(text);
        for (std::string line; std::getline(lines, line);)
            if (lower(trim(line)).rfind("error", 0) == 0) throw Error("Axis rejected the request: " + printable(text));
        if (!text.empty() && text.front() == '<') throw Error("Axis returned HTML/XML instead of a VAPIX PTZ response");
        return text;
    }

    CameraParams p_;
    std::string url_;
};

}  // namespace

std::unique_ptr<Camera> make_axis(const CameraParams &params, const Hooks &) {
    return std::make_unique<Axis>(params);
}

}  // namespace ptz
