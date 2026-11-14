// Locale-independent numbers, text helpers, SHA-1/Base64 for WS-Security, MD5 for RTSP Digest sign-in.
#include "internal.hpp"

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <locale>
#include <random>
#include <sstream>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#else
#include <arpa/inet.h>
#endif

namespace ptz {

std::optional<double> parse_number(std::string_view text) {
    std::string value = trim(text);
    if (value.empty() || value.size() > 64) return std::nullopt;
    for (char c : value) {
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '+' ||
              c == 'e' || c == 'E'))
            return std::nullopt;
    }
    std::istringstream stream(value);
    stream.imbue(std::locale::classic());
    double result = 0;
    stream >> result;
    if (stream.fail() || !stream.eof() || !std::isfinite(result)) return std::nullopt;
    return result;
}

std::string format_number(double value) {
    std::array<char, 64> buffer{};
    auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (error != std::errc()) throw Error("Cannot format number");
    return std::string(buffer.data(), end);
}

std::string lower(std::string_view text) {
    std::string result(text);
    for (char &c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

std::string trim(std::string_view text) {
    size_t start = 0, end = text.size();
    while (start < end && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(start, end - start));
}

std::string printable(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (unsigned char c : text) result.push_back(c < 0x20 || c == 0x7f ? ' ' : static_cast<char>(c));
    return result;
}

std::string base64_encode(const unsigned char *data, size_t size) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        uint32_t chunk = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < size) chunk |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < size) chunk |= data[i + 2];
        out.push_back(table[(chunk >> 18) & 63]);
        out.push_back(table[(chunk >> 12) & 63]);
        out.push_back(i + 1 < size ? table[(chunk >> 6) & 63] : '=');
        out.push_back(i + 2 < size ? table[chunk & 63] : '=');
    }
    return out;
}

void sha1(const unsigned char *data, size_t size, unsigned char digest[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::vector<unsigned char> message(data, data + size);
    uint64_t bits = static_cast<uint64_t>(size) * 8;
    message.push_back(0x80);
    while (message.size() % 64 != 56) message.push_back(0);
    for (int i = 7; i >= 0; --i) message.push_back(static_cast<unsigned char>(bits >> (i * 8)));
    auto rotl = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    for (size_t offset = 0; offset < message.size(); offset += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(message[offset + i * 4]) << 24) | (uint32_t(message[offset + i * 4 + 1]) << 16) |
                   (uint32_t(message[offset + i * 4 + 2]) << 8) | uint32_t(message[offset + i * 4 + 3]);
        for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = temp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) digest[i * 4 + j] = static_cast<unsigned char>(h[i] >> (24 - j * 8));
}

std::optional<std::string> base64_decode(std::string_view text) {
    std::string out;
    uint32_t chunk = 0;
    int bits = 0;
    for (char c : text) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=' || c == ' ' || c == '\r' || c == '\n') continue;
        else return std::nullopt;
        chunk = chunk << 6 | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((chunk >> bits) & 0xff));
        }
    }
    return out;
}

std::string md5_hex(std::string_view text) {
    static const uint32_t k[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int r[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    std::vector<unsigned char> message(text.begin(), text.end());
    uint64_t bits = static_cast<uint64_t>(text.size()) * 8;
    message.push_back(0x80);
    while (message.size() % 64 != 56) message.push_back(0);
    for (int i = 0; i < 8; ++i) message.push_back(static_cast<unsigned char>(bits >> (i * 8)));
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    for (size_t offset = 0; offset < message.size(); offset += 64) {
        uint32_t w[16];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(message[offset + i * 4]) | (uint32_t(message[offset + i * 4 + 1]) << 8) |
                   (uint32_t(message[offset + i * 4 + 2]) << 16) | (uint32_t(message[offset + i * 4 + 3]) << 24);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            uint32_t f;
            int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            uint32_t sum = a + f + k[i] + w[g];
            a = d; d = c; c = b;
            b = b + ((sum << r[i]) | (sum >> (32 - r[i])));
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    static const char hex[] = "0123456789abcdef";
    std::string out;
    for (uint32_t v : h)
        for (int i = 0; i < 4; ++i) {
            unsigned char byte = static_cast<unsigned char>(v >> (i * 8));
            out.push_back(hex[byte >> 4]);
            out.push_back(hex[byte & 15]);
        }
    return out;
}

std::string random_bytes(size_t count) {
#if defined(_WIN32)
    std::string bytes(count, '\0');
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(count),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw Error("Cannot obtain random bytes");
    return bytes;
#else
    // std::random_device is backed by the OS CSPRNG on macOS and Linux.
    std::random_device device;
    std::string bytes(count, '\0');
    for (auto &b : bytes) b = static_cast<char>(device() & 0xff);
    return bytes;
#endif
}

std::string bracket_host(const std::string &host) {
    return host.find(':') != std::string::npos ? "[" + host + "]" : host;
}

std::string validate_host(std::string value) {
    // Addresses go into URLs and SSH forwarding syntax, never into a shell.
    value = trim(value);
    if (!value.empty() && value.front() == '[' && value.back() == ']') value = value.substr(1, value.size() - 2);
    bool bad = value.empty() || value.front() == '-';
    for (unsigned char c : value)
        if (std::isspace(c) || c == '/' || c == '@' || c == '\\' || c == '?' || c == '#' || c < 0x20) bad = true;
    if (bad) throw Error("Enter an IP address or hostname, without a URL, username, or port");
    if (value.find(':') != std::string::npos) {
#if defined(_WIN32)
        static const bool winsock = [] {
            WSADATA data;
            return WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }();
        (void)winsock;
#endif
        unsigned char buffer[16];
        if (inet_pton(AF_INET6, value.c_str(), buffer) != 1)
            throw Error("Invalid IPv6 address (omit the port; set it in its own field)");
    }
    return value;
}

}  // namespace ptz
