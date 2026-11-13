// Still pictures of the camera's view: fetching them and reading their size.
#include "internal.hpp"

namespace ptz {

namespace {

unsigned big_endian(const std::string &data, size_t at, int bytes) {
    unsigned value = 0;
    for (int i = 0; i < bytes; ++i) value = value << 8 | static_cast<unsigned char>(data[at + i]);
    return value;
}

}  // namespace

std::optional<std::pair<int, int>> picture_size(const std::string &data) {
    // PNG: the IHDR chunk comes first.
    if (data.size() >= 24 && data.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0 && data.compare(12, 4, "IHDR") == 0)
        return std::make_pair(static_cast<int>(big_endian(data, 16, 4)), static_cast<int>(big_endian(data, 20, 4)));
    // JPEG: walk the markers to the first start-of-frame.
    if (data.size() < 4 || static_cast<unsigned char>(data[0]) != 0xFF || static_cast<unsigned char>(data[1]) != 0xD8)
        return std::nullopt;
    size_t at = 2;
    while (at + 4 <= data.size()) {
        if (static_cast<unsigned char>(data[at]) != 0xFF) return std::nullopt;
        unsigned marker = static_cast<unsigned char>(data[at + 1]);
        if (marker == 0xFF) { ++at; continue; }  // fill byte
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) { at += 2; continue; }
        unsigned length = big_endian(data, at + 2, 2);
        bool start_of_frame = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (start_of_frame && at + 9 <= data.size())
            return std::make_pair(static_cast<int>(big_endian(data, at + 7, 2)), static_cast<int>(big_endian(data, at + 5, 2)));
        if (length < 2) return std::nullopt;
        at += 2 + length;
    }
    return std::nullopt;
}

std::string fetch_picture(const std::string &url, const std::string &user, const std::string &password, double timeout) {
    HttpRequest http;
    http.url = url;
    http.timeout = timeout;
    // Digest only, as for every other camera request: the password never goes out in clear Basic form.
    http.digest_auth = true;
    http.user = user;
    http.password = password;
    http.max_body = 16 * 1024 * 1024;
    HttpResponse response = http_perform(http);
    if (!response.transport_ok) throw Error("Camera unreachable: " + response.transport_error);
    if (response.status == 401) {
        if (lower(response.headers["www-authenticate"]).find("digest") == std::string::npos)
            throw Error("The camera asks for Basic sign-in for its picture, which sends the password unprotected; "
                        "turn on Digest sign-in on the camera");
        throw Error("Camera rejected the credentials for its picture (HTTP 401)");
    }
    if (response.status >= 300 && response.status < 400)
        throw Error("Camera redirected the picture request (HTTPS is not supported)");
    if (response.status >= 400) throw Error("Camera returned HTTP " + std::to_string(response.status) + " for its picture");
    auto size = picture_size(response.body);
    if (!size || size->first <= 0 || size->second <= 0)
        throw Error("Camera did not send a JPEG or PNG picture");
    return response.body;
}

}  // namespace ptz
