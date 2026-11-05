// libcurl transport. Proxies from the environment are never used for camera
// traffic, and redirects are never followed automatically.
#include "internal.hpp"

#if !defined(_WIN32)

#include <curl/curl.h>

#include <mutex>

#include <unistd.h>

namespace ptz {

namespace {

#if defined(__linux__)
const char *system_ca_bundle() {
    static const char *found = [] () -> const char * {
        for (const char *path : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                                 "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem", "/etc/pki/tls/cacert.pem"})
            if (access(path, R_OK) == 0) return path;
        return nullptr;
    }();
    return found;
}
#endif

struct Sink {
    HttpResponse *response;
    size_t limit;
    bool truncated = false;
};

size_t on_body(char *data, size_t size, size_t count, void *user) {
    auto *sink = static_cast<Sink *>(user);
    size_t bytes = size * count;
    size_t room = sink->limit - std::min(sink->limit, sink->response->body.size());
    if (bytes > room) {
        sink->response->body.append(data, room);
        sink->truncated = true;
        return 0;  // stop the transfer; the status line is already known
    }
    sink->response->body.append(data, bytes);
    return bytes;
}

size_t on_header(char *data, size_t size, size_t count, void *user) {
    auto *response = static_cast<HttpResponse *>(user);
    std::string line(data, size * count);
    if (line.rfind("HTTP/", 0) == 0) response->headers.clear();  // new response after a 401 challenge
    auto colon = line.find(':');
    if (colon != std::string::npos)
        response->headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    return size * count;
}

}  // namespace

void http_global_init() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

HttpResponse http_perform(const HttpRequest &request) {
    http_global_init();
    HttpResponse response;
    CURL *curl = curl_easy_init();
    if (!curl) {
        response.transport_error = "Cannot initialise HTTP client";
        return response;
    }
    Sink sink{&response, request.max_body};
    curl_slist *headers = nullptr;
    for (const auto &h : request.headers) headers = curl_slist_append(headers, h.c_str());
    char error[CURL_ERROR_SIZE] = {0};

    long timeout_ms = static_cast<long>(request.timeout * 1000);
    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, std::min(timeout_ms, 10000L));
#if LIBCURL_VERSION_NUM >= 0x075500  // 7.85
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
#if defined(__linux__)
    // A bundled libcurl only knows its build distribution's CA path; use this system's.
    if (const char *bundle = system_ca_bundle()) curl_easy_setopt(curl, CURLOPT_CAINFO, bundle);
#endif
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "PTZControl/1.0");
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response);
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (request.method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
    }
    if (request.digest_auth) {
        // Digest only: credentials are never sent in clear Basic form.
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
        curl_easy_setopt(curl, CURLOPT_USERNAME, request.user.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, request.password.c_str());
    }

    CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    if (code == CURLE_OK || (code == CURLE_WRITE_ERROR && sink.truncated)) {
        response.transport_ok = true;
    } else {
        response.tls_verify_failed = code == CURLE_PEER_FAILED_VERIFICATION;
        response.transport_error = *error ? error : curl_easy_strerror(code);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

}  // namespace ptz

#endif
