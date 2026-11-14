// Live video: a small RTSP client that plays a camera's H.264 or H.265 stream over one TCP connection (RTP
// interleaved, RFC 2326 §10.12), signs in with Digest only, and turns RTP packets back into whole frames
// (RFC 6184 for H.264, RFC 7798 for H.265). Decoding is left to each platform's own video decoder.
#include "internal.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace ptz {

UriParts split_uri(const std::string &uri) {
    UriParts parts;
    auto scheme_end = uri.find("://");
    if (scheme_end == std::string::npos) throw Error("Camera gave an invalid video address");
    parts.scheme = lower(uri.substr(0, scheme_end));
    size_t authority_start = scheme_end + 3;
    size_t path_start = uri.find_first_of("/?", authority_start);
    std::string authority = uri.substr(authority_start, path_start == std::string::npos ? std::string::npos
                                                                                          : path_start - authority_start);
    parts.path = path_start == std::string::npos ? "/" : uri.substr(path_start);
    if (!parts.path.empty() && parts.path.front() == '?') parts.path = "/" + parts.path;
    auto at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    std::string port;
    if (!authority.empty() && authority.front() == '[') {
        auto close = authority.find(']');
        if (close == std::string::npos) throw Error("Camera gave an invalid video address");
        parts.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == ':') port = authority.substr(close + 2);
    } else {
        auto colon = authority.find(':');
        parts.host = authority.substr(0, colon);
        if (colon != std::string::npos) port = authority.substr(colon + 1);
    }
    if (parts.host.empty()) throw Error("Camera gave an invalid video address");
    if (!port.empty()) {
        auto value = parse_number(port);
        if (!value || *value < 1 || *value > 65535 || *value != static_cast<int>(*value))
            throw Error("Camera gave an invalid video port");
        parts.port = static_cast<int>(*value);
    }
    return parts;
}

namespace {

#if defined(_WIN32)
using Socket = SOCKET;
const Socket kNoSocket = INVALID_SOCKET;
void close_socket(Socket s) { closesocket(s); }
bool sockets_ready() {
    static const bool ready = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}
bool timed_out() { return WSAGetLastError() == WSAETIMEDOUT; }
#else
using Socket = int;
const Socket kNoSocket = -1;
void close_socket(Socket s) { ::close(s); }
bool sockets_ready() { return true; }
bool timed_out() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
#endif

using Clock = std::chrono::steady_clock;

struct Response {
    int status = 0;
    std::map<std::string, std::vector<std::string>> headers;  // lower-case names
    std::string body;
    std::string header(const std::string &name) const {
        auto it = headers.find(name);
        return it == headers.end() || it->second.empty() ? "" : it->second.front();
    }
};

// Splits "a=1, b=\"x, y\"" into its parameters, as in Digest challenges.
std::map<std::string, std::string> auth_parameters(const std::string &text) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == ',')) ++i;
        size_t eq = text.find('=', i);
        if (eq == std::string::npos) break;
        std::string name = lower(trim(text.substr(i, eq - i)));
        i = eq + 1;
        std::string value;
        if (i < text.size() && text[i] == '"') {
            ++i;
            while (i < text.size() && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < text.size()) ++i;
                value.push_back(text[i++]);
            }
            ++i;
        } else {
            size_t end = text.find(',', i);
            value = trim(text.substr(i, end == std::string::npos ? std::string::npos : end - i));
            i = end == std::string::npos ? text.size() : end;
        }
        out[name] = value;
    }
    return out;
}

// Rebuilds frames from RTP packets.
class Depacketizer {
public:
    Depacketizer(Codec codec, std::vector<std::string> parameter_sets) : codec_(codec) {
        for (auto &nal : parameter_sets) remember(nal);
    }

    // Takes one RTP packet; calls on_frame when it completes a frame.
    void packet(const unsigned char *data, size_t size,
                const std::function<void(const std::string &, bool, double)> &on_frame) {
        if (size < 12 || (data[0] >> 6) != 2) return;
        bool marker = data[1] & 0x80;
        uint16_t sequence = static_cast<uint16_t>(data[2] << 8 | data[3]);
        uint32_t timestamp = uint32_t(data[4]) << 24 | uint32_t(data[5]) << 16 | uint32_t(data[6]) << 8 | data[7];
        size_t at = 12 + 4 * (data[0] & 0x0f);
        if (data[0] & 0x10) {  // header extension
            if (at + 4 > size) return;
            at += 4 + 4 * (size_t(data[at + 2]) << 8 | data[at + 3]);
        }
        size_t end = size;
        if (data[0] & 0x20) {  // padding
            if (data[size - 1] > size - at) return;
            end -= data[size - 1];
        }
        if (at >= end) return;
        if (have_sequence_ && sequence != static_cast<uint16_t>(last_sequence_ + 1)) {
            nals_.clear();  // a lost packet spoils the frame being built
            frame_bytes_ = 0;
            fragment_.clear();
            in_fragment_ = false;
            broken_ = true;
        }
        have_sequence_ = true;
        last_sequence_ = sequence;
        if (!nals_.empty() && timestamp != frame_timestamp_) finish(on_frame);
        frame_timestamp_ = timestamp;
        payload(data + at, end - at);
        if (marker) finish(on_frame);
    }

private:
    void payload(const unsigned char *p, size_t n) {
        if (codec_ == Codec::h264) {
            int type = p[0] & 0x1f;
            if (type >= 1 && type <= 23) {
                add(std::string(reinterpret_cast<const char *>(p), n));
            } else if (type == 24) {  // STAP-A
                aggregate(p + 1, n - 1);
            } else if (type == 28 && n >= 2) {  // FU-A
                bool start = p[1] & 0x80, stop = p[1] & 0x40;
                if (start) {
                    fragment_.assign(1, static_cast<char>((p[0] & 0xe0) | (p[1] & 0x1f)));
                    in_fragment_ = true;
                }
                if (!in_fragment_) return;
                if (fragment_.size() > kMaxFrame) { in_fragment_ = false; fragment_.clear(); return; }
                fragment_.append(reinterpret_cast<const char *>(p + 2), n - 2);
                if (stop) {
                    add(fragment_);
                    fragment_.clear();
                    in_fragment_ = false;
                }
            }
        } else {
            if (n < 2) return;
            int type = (p[0] >> 1) & 0x3f;
            if (type < 48) {
                add(std::string(reinterpret_cast<const char *>(p), n));
            } else if (type == 48) {  // aggregation packet
                aggregate(p + 2, n - 2);
            } else if (type == 49 && n >= 3) {  // fragmentation unit
                bool start = p[2] & 0x80, stop = p[2] & 0x40;
                if (start) {
                    fragment_.clear();
                    fragment_.push_back(static_cast<char>((p[0] & 0x81) | ((p[2] & 0x3f) << 1)));
                    fragment_.push_back(static_cast<char>(p[1]));
                    in_fragment_ = true;
                }
                if (!in_fragment_) return;
                if (fragment_.size() > kMaxFrame) { in_fragment_ = false; fragment_.clear(); return; }
                fragment_.append(reinterpret_cast<const char *>(p + 3), n - 3);
                if (stop) {
                    add(fragment_);
                    fragment_.clear();
                    in_fragment_ = false;
                }
            }
        }
    }

    void aggregate(const unsigned char *p, size_t n) {
        size_t i = 0;
        while (i + 2 <= n) {
            size_t length = size_t(p[i]) << 8 | p[i + 1];
            i += 2;
            if (length == 0 || i + length > n) return;
            add(std::string(reinterpret_cast<const char *>(p + i), length));
            i += length;
        }
    }

    int type_of(const std::string &nal) const {
        auto b = static_cast<unsigned char>(nal[0]);
        return codec_ == Codec::h264 ? (b & 0x1f) : ((b >> 1) & 0x3f);
    }
    bool is_parameter_set(int type) const {
        return codec_ == Codec::h264 ? (type == 7 || type == 8) : (type >= 32 && type <= 34);
    }
    bool is_key(int type) const { return codec_ == Codec::h264 ? type == 5 : (type >= 16 && type <= 21); }

    void remember(const std::string &nal) {
        if (nal.empty()) return;
        int type = type_of(nal);
        if (is_parameter_set(type)) parameter_sets_[type] = nal;
    }

    void add(const std::string &nal) {
        if (nal.empty()) return;
        remember(nal);
        frame_bytes_ += nal.size();
        if (frame_bytes_ > kMaxFrame) {  // a frame that never ends: drop it and wait for a key frame
            nals_.clear();
            frame_bytes_ = 0;
            broken_ = true;
            return;
        }
        nals_.push_back(nal);
    }

    void finish(const std::function<void(const std::string &, bool, double)> &on_frame) {
        std::vector<std::string> nals;
        nals.swap(nals_);
        frame_bytes_ = 0;
        fragment_.clear();
        in_fragment_ = false;
        bool key = false, has_picture = false;
        std::map<int, bool> present;
        for (auto &nal : nals) {
            int type = type_of(nal);
            present[type] = true;
            if (is_key(type)) key = true;
            if (codec_ == Codec::h264 ? (type >= 1 && type <= 5) : type < 32) has_picture = true;
        }
        if (!has_picture) return;
        if (key) broken_ = false;
        if (broken_ || (!key && !started_)) return;  // wait for a key frame
        static const char start_code[] = {0, 0, 0, 1};
        std::string frame;
        if (key)
            for (auto &[type, nal] : parameter_sets_)
                if (!present[type]) frame.append(start_code, 4).append(nal);
        for (auto &nal : nals) frame.append(start_code, 4).append(nal);
        if (!started_) {
            started_ = true;
            first_timestamp_ = frame_timestamp_;
        }
        double time = static_cast<uint32_t>(frame_timestamp_ - first_timestamp_) / 90000.0;
        on_frame(frame, key, time);
    }

    static constexpr size_t kMaxFrame = 32 * 1024 * 1024;
    Codec codec_;
    size_t frame_bytes_ = 0;
    std::map<int, std::string> parameter_sets_;  // by NAL type, so VPS, SPS, PPS keep their order
    std::vector<std::string> nals_;
    std::string fragment_;
    bool in_fragment_ = false;
    bool have_sequence_ = false, started_ = false, broken_ = false;
    uint16_t last_sequence_ = 0;
    uint32_t frame_timestamp_ = 0, first_timestamp_ = 0;
};

}  // namespace

struct RtspStream::Impl {
    StreamTarget t;
    Socket socket = kNoSocket;
    std::string buffer;
    size_t read_at = 0;
    int cseq = 0;
    std::string session;
    double session_timeout = 60;
    std::map<std::string, std::string> digest;  // the camera's challenge, once known
    int nonce_count = 0;
    Codec codec = Codec::h264;
    int payload_type = -1;
    int video_channel = 0;
    std::vector<std::string> parameter_sets;
    std::string control_uri, base_uri;
    std::string keepalive = "GET_PARAMETER";

    ~Impl() {
        if (socket != kNoSocket) close_socket(socket);
    }

    // A connect that gives up after the request timeout, not the system's own (over a minute on a filtered port).
    bool connect_within(Socket s, const sockaddr *address, int length) {
#if defined(_WIN32)
        u_long nonblocking = 1;
        ioctlsocket(s, FIONBIO, &nonblocking);
        bool started = ::connect(s, address, length) == 0 || WSAGetLastError() == WSAEWOULDBLOCK;
#else
        int flags = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
        bool started = ::connect(s, address, static_cast<socklen_t>(length)) == 0 || errno == EINPROGRESS;
#endif
        if (!started) return false;
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(s, &writable);
        FD_SET(s, &failed);
        timeval wait{static_cast<long>(t.timeout), static_cast<int>((t.timeout - static_cast<long>(t.timeout)) * 1e6)};
        if (select(static_cast<int>(s) + 1, nullptr, &writable, &failed, &wait) <= 0 || FD_ISSET(s, &failed))
            return false;
        int error = 0;
        socklen_t size = sizeof error;
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &size);
        if (error != 0) return false;
#if defined(_WIN32)
        nonblocking = 0;
        ioctlsocket(s, FIONBIO, &nonblocking);
#else
        fcntl(s, F_SETFL, flags);
#endif
        return true;
    }

    void connect_socket() {
        if (!sockets_ready()) throw Error("Cannot start Windows networking for the live view");
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *found = nullptr;
        if (getaddrinfo(t.dial_host.c_str(), std::to_string(t.dial_port).c_str(), &hints, &found) != 0 || !found)
            throw Error("Cannot find the camera's video address " + t.dial_host);
        for (addrinfo *a = found; a; a = a->ai_next) {
            Socket s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s == kNoSocket) continue;
            if (connect_within(s, a->ai_addr, static_cast<int>(a->ai_addrlen))) {
                socket = s;
                break;
            }
            close_socket(s);
        }
        freeaddrinfo(found);
        if (socket == kNoSocket)
            throw Error("Cannot reach the camera's video port " + std::to_string(t.dial_port));
        // Short receive waits let the reader send keep-alives and notice a stop.
#if defined(_WIN32)
        DWORD wait = 500;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&wait), sizeof wait);
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&wait), sizeof wait);
#else
        timeval wait{0, 500000};
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof wait);
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &wait, sizeof wait);
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
#endif
    }

    void send_all(const std::string &text) {
        size_t sent = 0;
        auto deadline = Clock::now() + std::chrono::duration<double>(t.timeout);
        while (sent < text.size()) {
#if defined(_WIN32)
            int n = ::send(socket, text.data() + sent, static_cast<int>(text.size() - sent), 0);
#elif defined(MSG_NOSIGNAL)
            ssize_t n = ::send(socket, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
#else
            ssize_t n = ::send(socket, text.data() + sent, text.size() - sent, 0);
#endif
            if (n > 0) {
                sent += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && timed_out() && Clock::now() < deadline) continue;
            throw Error("The camera closed the video connection");
        }
    }

    // Reads more bytes; false when the wait ran out without any.
    bool fill() {
        if (read_at > 65536 && read_at * 2 > buffer.size()) {
            buffer.erase(0, read_at);
            read_at = 0;
        }
        char chunk[16384];
#if defined(_WIN32)
        int n = ::recv(socket, chunk, sizeof chunk, 0);
#else
        ssize_t n = ::recv(socket, chunk, sizeof chunk, 0);
#endif
        if (n > 0) {
            buffer.append(chunk, static_cast<size_t>(n));
            return true;
        }
        if (n < 0 && timed_out()) return false;
        throw Error("The camera closed the video connection");
    }

    size_t available() const { return buffer.size() - read_at; }

    // Parses one RTSP message at read_at if it is complete.
    std::optional<Response> take_message() {
        size_t head_end = buffer.find("\r\n\r\n", read_at);
        if (head_end == std::string::npos) {
            if (available() > 65536) throw Error("The camera sent a malformed video reply");
            return std::nullopt;
        }
        std::string head = buffer.substr(read_at, head_end - read_at);
        Response response;
        size_t line_end = head.find("\r\n");
        std::string status_line = head.substr(0, line_end);
        if (status_line.rfind("RTSP/", 0) == 0) {
            auto space = status_line.find(' ');
            if (space != std::string::npos) response.status = std::atoi(status_line.c_str() + space + 1);
        }
        size_t at = line_end == std::string::npos ? head.size() : line_end + 2;
        while (at < head.size()) {
            size_t end = head.find("\r\n", at);
            if (end == std::string::npos) end = head.size();
            std::string line = head.substr(at, end - at);
            auto colon = line.find(':');
            if (colon != std::string::npos)
                response.headers[lower(trim(line.substr(0, colon)))].push_back(trim(line.substr(colon + 1)));
            at = end + 2;
        }
        size_t length = 0;
        if (auto value = parse_number(response.header("content-length"))) {
            if (*value < 0 || *value > 1 << 20) throw Error("The camera sent a malformed video reply");
            length = static_cast<size_t>(*value);
        }
        if (head_end + 4 + length > buffer.size()) return std::nullopt;
        response.body = buffer.substr(head_end + 4, length);
        read_at = head_end + 4 + length;
        return response;
    }

    // Whether a qop list ("auth,auth-int") offers plain auth.
    static bool offers_auth(const std::string &qop) {
        size_t start = 0;
        while (start <= qop.size()) {
            size_t comma = qop.find(',', start);
            if (lower(trim(qop.substr(start, comma == std::string::npos ? std::string::npos : comma - start))) == "auth")
                return true;
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return false;
    }

    // Takes a Digest challenge this client can answer: MD5 (the default), with qop auth or none.
    bool take_challenge(const std::string &challenge) {
        auto parameters = auth_parameters(challenge.substr(6));
        std::string algorithm = lower(parameters["algorithm"]);
        if (!algorithm.empty() && algorithm != "md5") return false;
        std::string qop = parameters["qop"];
        if (!qop.empty() && !offers_auth(qop)) return false;  // auth-int alone isn't supported
        digest = parameters;
        nonce_count = 0;
        return true;
    }

    std::string authorization(const std::string &method, const std::string &uri) {
        if (digest.empty()) return "";
        const std::string &realm = digest["realm"], &nonce = digest["nonce"];
        std::string ha1 = md5_hex(t.user + ":" + realm + ":" + t.password);
        std::string ha2 = md5_hex(method + ":" + uri);
        std::string header = "Authorization: Digest username=\"" + t.user + "\", realm=\"" + realm + "\", nonce=\"" +
                             nonce + "\", uri=\"" + uri + "\"";
        if (offers_auth(digest["qop"])) {
            char nc[9];
            std::snprintf(nc, sizeof nc, "%08x", ++nonce_count);
            std::string raw = random_bytes(8), cnonce;
            static const char hex[] = "0123456789abcdef";
            for (unsigned char c : raw) cnonce += {hex[c >> 4], hex[c & 15]};
            header += ", qop=auth, nc=" + std::string(nc) + ", cnonce=\"" + cnonce + "\", response=\"" +
                      md5_hex(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":auth:" + ha2) + "\"";
        } else {
            header += ", response=\"" + md5_hex(ha1 + ":" + nonce + ":" + ha2) + "\"";
        }
        if (digest.count("opaque")) header += ", opaque=\"" + digest["opaque"] + "\"";
        if (!digest["algorithm"].empty()) header += ", algorithm=MD5";
        return header + "\r\n";
    }

    std::string request_text(const std::string &method, const std::string &uri, const std::string &extra) {
        std::string text = method + " " + uri + " RTSP/1.0\r\nCSeq: " + std::to_string(++cseq) +
                           "\r\nUser-Agent: PTZ Control\r\n" + authorization(method, uri);
        if (!session.empty()) text += "Session: " + session + "\r\n";
        return text + extra + "\r\n";
    }

    // Sends a request during setup and waits for its reply, signing in once when challenged.
    Response request(const std::string &method, const std::string &uri, const std::string &extra = "") {
        for (int attempt = 0; attempt < 2; ++attempt) {
            send_all(request_text(method, uri, extra));
            Response response = wait_reply();
            if (response.status != 401 || attempt == 1) return response;
            bool found = false, basic = false;
            for (auto &challenge : response.headers["www-authenticate"]) {
                std::string l = lower(challenge);
                if (l.rfind("digest", 0) == 0 && take_challenge(challenge)) {
                    found = true;
                    break;
                }
                if (l.rfind("basic", 0) == 0) basic = true;
            }
            if (!found) {
                if (basic)
                    throw Error("The camera asks for Basic sign-in for its video, which sends the password "
                                "unprotected; turn on Digest sign-in for RTSP on the camera");
                throw Error("The camera refused the video request (RTSP 401)");
            }
        }
        throw Error("The camera refused the video request");
    }

    Response wait_reply() {
        auto deadline = Clock::now() + std::chrono::duration<double>(t.timeout);
        for (;;) {
            // Skip any interleaved data that arrives before the reply.
            while (available() >= 1 && buffer[read_at] == '$') {
                if (available() < 4) break;
                size_t length = size_t(static_cast<unsigned char>(buffer[read_at + 2])) << 8 |
                                static_cast<unsigned char>(buffer[read_at + 3]);
                if (available() < 4 + length) break;
                read_at += 4 + length;
            }
            if (available() && buffer[read_at] != '$')
                if (auto response = take_message()) return *response;
            if (Clock::now() > deadline) throw Error("The camera didn't answer the video request in time");
            fill();
        }
    }

    static std::string status_reason(const std::string &method, int status) {
        if (status == 401) return "The camera rejected the credentials for its video (RTSP 401)";
        if (status == 404) return "The camera has no video at the address it gave (RTSP 404)";
        if (status == 461) return "The camera won't send its video over TCP (RTSP 461)";
        return "The camera answered " + method + " with RTSP " + std::to_string(status);
    }

    void read_sdp(const std::string &sdp, const std::string &base) {
        bool in_video = false, found = false;
        std::string control, fmtp;
        size_t at = 0;
        while (at < sdp.size()) {
            size_t end = sdp.find('\n', at);
            if (end == std::string::npos) end = sdp.size();
            std::string line = trim(sdp.substr(at, end - at));
            at = end + 1;
            if (line.rfind("m=", 0) == 0) {
                if (found) break;  // only the first video section
                in_video = line.rfind("m=video", 0) == 0;
                if (in_video) {
                    // m=video 0 RTP/AVP 96
                    auto last = line.rfind(' ');
                    payload_type = std::atoi(line.c_str() + last + 1);
                    found = true;
                }
                continue;
            }
            if (!in_video) continue;
            if (line.rfind("a=control:", 0) == 0) control = line.substr(10);
            std::string pt = std::to_string(payload_type);
            if (line.rfind("a=rtpmap:" + pt + " ", 0) == 0) {
                std::string encoding = lower(line.substr(10 + pt.size()));
                if (encoding.rfind("h264/", 0) == 0) codec = Codec::h264;
                else if (encoding.rfind("h265/", 0) == 0 || encoding.rfind("hevc/", 0) == 0) codec = Codec::h265;
                else if (encoding.rfind("jpeg/", 0) == 0)
                    throw Error("The camera sends Motion JPEG video, which the live view doesn't play; choose a "
                                "profile with H.264 or H.265 video");
                else
                    throw Error("The camera sends video as " + encoding.substr(0, encoding.find('/')) +
                                ", which the live view doesn't play; choose a profile with H.264 or H.265 video");
            }
            if (line.rfind("a=fmtp:" + pt + " ", 0) == 0) fmtp = line.substr(8 + pt.size());
        }
        if (!found) throw Error("The camera's stream has no video");
        if (payload_type == 26)
            throw Error("The camera sends Motion JPEG video, which the live view doesn't play; choose a profile "
                        "with H.264 or H.265 video");
        // fmtp parameters are separated by semicolons; sprop-parameter-sets holds a comma-separated list.
        std::map<std::string, std::string> fmtp_parameters;
        for (size_t start = 0; start < fmtp.size();) {
            size_t semicolon = fmtp.find(';', start);
            std::string item = fmtp.substr(start, semicolon == std::string::npos ? std::string::npos : semicolon - start);
            auto eq = item.find('=');
            if (eq != std::string::npos) fmtp_parameters[lower(trim(item.substr(0, eq)))] = trim(item.substr(eq + 1));
            if (semicolon == std::string::npos) break;
            start = semicolon + 1;
        }
        for (auto &[name, value] : fmtp_parameters) {
            auto add = [&](const std::string &encoded) {
                if (auto nal = base64_decode(encoded); nal && !nal->empty()) parameter_sets.push_back(*nal);
            };
            if (name == "sprop-parameter-sets") {
                size_t start = 0;
                while (start <= value.size()) {
                    size_t comma = value.find(',', start);
                    add(value.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
            } else if (name == "sprop-vps" || name == "sprop-sps" || name == "sprop-pps") {
                add(value);
            }
        }
        if (control.empty() || control == "*") control_uri = base;
        else if (lower(control).rfind("rtsp://", 0) == 0) control_uri = control;
        else control_uri = base + (base.empty() || base.back() == '/' ? "" : "/") + control;
    }

    void open() {
        connect_socket();
        try {
            negotiate();
        } catch (...) {
            if (!session.empty()) {
                try {
                    send_all(request_text("TEARDOWN", base_uri.empty() ? t.uri : base_uri, ""));
                } catch (...) {
                }
            }
            throw;
        }
    }

    void negotiate() {
        Response options = request("OPTIONS", t.uri);
        if (options.status == 401) throw Error(status_reason("OPTIONS", 401));
        Response describe = request("DESCRIBE", t.uri, "Accept: application/sdp\r\n");
        if (describe.status != 200) throw Error(status_reason("DESCRIBE", describe.status));
        std::string base = describe.header("content-base");
        if (base.empty()) base = describe.header("content-location");
        if (base.empty()) base = t.uri;
        base_uri = base;
        read_sdp(describe.body, base);
        Response setup = request("SETUP", control_uri, "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n");
        if (setup.status != 200) throw Error(status_reason("SETUP", setup.status));
        std::string session_header = setup.header("session");
        if (session_header.empty()) throw Error("The camera didn't start a video session");
        session = trim(session_header.substr(0, session_header.find(';')));
        auto timeout_at = lower(session_header).find("timeout=");
        if (timeout_at != std::string::npos) {
            double value = std::atof(session_header.c_str() + timeout_at + 8);
            if (value >= 5) session_timeout = value;
        }
        std::string transport = lower(setup.header("transport"));
        auto interleaved = transport.find("interleaved=");
        if (interleaved != std::string::npos) video_channel = std::atoi(transport.c_str() + interleaved + 12);
        else if (transport.find("/tcp") == std::string::npos)
            throw Error("The camera won't send its video over TCP");
        Response play = request("PLAY", base_uri, "Range: npt=0.000-\r\n");
        if (play.status != 200) throw Error(status_reason("PLAY", play.status));
    }

    void run(const std::function<void(const std::string &, bool, double)> &on_frame, const std::atomic<bool> &stop) {
        Depacketizer depacketizer(codec, parameter_sets);
        auto last_data = Clock::now(), last_keepalive = Clock::now();
        double silence = std::max(10.0, t.timeout);
        while (!stop) {
            // Always read: what is buffered may end partway through a packet.
            bool got = fill();
            auto now = Clock::now();
            if (std::chrono::duration<double>(now - last_keepalive).count() > session_timeout / 2) {
                send_all(request_text(keepalive, t.uri, ""));
                last_keepalive = now;
            }
            if (!got) {
                if (std::chrono::duration<double>(now - last_data).count() > silence)
                    throw Error("The camera stopped sending video");
                continue;
            }
            for (;;) {
                if (available() >= 1 && buffer[read_at] == '$') {
                    if (available() < 4) break;
                    int channel = static_cast<unsigned char>(buffer[read_at + 1]);
                    size_t length = size_t(static_cast<unsigned char>(buffer[read_at + 2])) << 8 |
                                    static_cast<unsigned char>(buffer[read_at + 3]);
                    if (available() < 4 + length) break;
                    if (channel == video_channel) {
                        last_data = now;
                        depacketizer.packet(reinterpret_cast<const unsigned char *>(buffer.data() + read_at + 4),
                                            length, on_frame);
                    }
                    read_at += 4 + length;
                } else if (available() >= 1) {
                    auto message = take_message();
                    if (!message) break;
                    // A camera that doesn't know GET_PARAMETER is kept alive with OPTIONS; one whose sign-in has
                    // expired gets the keep-alive again with its new challenge.
                    if (message->status == 405 || message->status == 501) keepalive = "OPTIONS";
                    if (message->status == 401) {
                        std::string old_nonce = digest["nonce"];
                        for (auto &challenge : message->headers["www-authenticate"])
                            if (lower(challenge).rfind("digest", 0) == 0 && take_challenge(challenge)) {
                                if (digest["nonce"] != old_nonce) last_keepalive = Clock::time_point{};  // resend now
                                break;
                            }
                    }
                } else {
                    break;
                }
                if (stop) return;
            }
        }
    }
};

RtspStream::RtspStream(StreamTarget target) : impl_(std::make_unique<Impl>()) { impl_->t = std::move(target); }
RtspStream::~RtspStream() = default;
void RtspStream::open() { impl_->open(); }
Codec RtspStream::codec() const { return impl_->codec; }
void RtspStream::run(const std::function<void(const std::string &, bool, double)> &on_frame,
                     const std::atomic<bool> &stop) {
    impl_->run(on_frame, stop);
}
void RtspStream::interrupt() {
    if (impl_->socket != kNoSocket) {
#if defined(_WIN32)
        shutdown(impl_->socket, SD_RECEIVE);
#else
        shutdown(impl_->socket, SHUT_RD);
#endif
    }
}
void RtspStream::teardown() {
    if (impl_->socket == kNoSocket || impl_->session.empty()) return;
    try {
        impl_->send_all(impl_->request_text("TEARDOWN", impl_->t.uri, ""));
    } catch (...) {
    }
}

}  // namespace ptz
