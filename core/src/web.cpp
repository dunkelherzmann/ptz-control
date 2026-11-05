// Locate the camera's own web UI. Probes send no credentials and pages are
// never proxied; the host opens the resulting URL in the user's browser.
#include "internal.hpp"

#include <algorithm>

namespace ptz {

namespace {

struct Url {
    std::string scheme, host, path;
    int port = 0;
    bool userinfo = false;
};

Url split(const std::string &text) {
    Url url;
    auto scheme_end = text.find("://");
    if (scheme_end == std::string::npos) return url;
    url.scheme = lower(text.substr(0, scheme_end));
    auto rest = text.substr(scheme_end + 3);
    auto path_start = rest.find_first_of("/?#");
    std::string authority = rest.substr(0, path_start);
    url.path = path_start == std::string::npos ? "/" : rest.substr(path_start);
    if (auto at = authority.rfind('@'); at != std::string::npos) {
        url.userinfo = true;
        authority = authority.substr(at + 1);
    }
    std::string port;
    if (!authority.empty() && authority[0] == '[') {
        auto close = authority.find(']');
        url.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':') port = authority.substr(close + 2);
    } else if (auto colon = authority.rfind(':'); colon != std::string::npos) {
        url.host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    } else {
        url.host = authority;
    }
    url.port = port.empty() ? (url.scheme == "https" ? 443 : 80) : std::atoi(port.c_str());
    return url;
}

}  // namespace

std::string WebInterface::address(const std::string &scheme, int port) const {
    std::string host = target_.tunnel ? "127.0.0.1" : target_.camera_host;
    return scheme + "://" + bracket_host(host) + ":" + std::to_string(port) + "/";
}

WebInterface::Probe WebInterface::probe(const std::string &url) {
    Probe result;
    HttpRequest request;
    request.url = url;
    request.timeout = std::min(target_.timeout, 4.0);
    request.max_body = 1;
    HttpResponse response = http_perform(request);
    if (!response.transport_ok) {
        if (response.tls_verify_failed) {
            result.reachable = true;
            result.final_url = url;
            result.notice = "Verify the camera certificate in your browser.";
        }
        return result;
    }
    long status = response.status;
    if (status >= 300 && status < 400) {
        std::string location = response.headers["location"];
        if (!location.empty() && location[0] == '/') location = url.substr(0, url.find('/', url.find("://") + 3)) + location;
        Url to = split(location), from = split(url);
        std::string host = lower(to.host);
        if (to.userinfo || (host != lower(target_.camera_host) && host != "127.0.0.1" && !host.empty()))
            throw Error("Camera redirected outside the configured host; check its web settings");
        if (to.scheme != "http" && to.scheme != "https")
            throw Error("Camera web redirect uses an unsupported protocol");
        if (from.scheme == "http" && to.scheme == "https") {
            int port = to.port;
            if (target_.tunnel && host == "127.0.0.1" && port == target_.local_http_port) port = target_.https_port;
            result.https_redirect_port = port;
            return result;
        }
        result.reachable = true;
        result.final_url = target_.tunnel ? from.scheme + "://127.0.0.1:" + std::to_string(from.port) + to.path : location;
        return result;
    }
    // Login challenges mean the web interface is reachable.
    result.reachable = status < 400 || status == 401 || status == 403;
    if (result.reachable) result.final_url = url;
    return result;
}

WebResult WebInterface::locate() {
    Probe http = probe(address("http", target_.local_http_port));
    if (http.reachable) return {http.final_url, http.notice};
    int camera_port = http.https_redirect_port.value_or(target_.https_port);
    int local_port = camera_port;
    if (target_.tunnel) {
        if (!https_local_port_ || https_camera_port_ != camera_port) {
            https_local_port_ = target_.tunnel->forward(camera_port);
            https_camera_port_ = camera_port;
        }
        local_port = *https_local_port_;
    }
    Probe https = probe(address("https", local_port));
    if (!https.reachable)
        throw Error("Camera web interface unavailable over HTTP and HTTPS; check its ports and network access");
    return {https.final_url, https.notice};
}

}  // namespace ptz
