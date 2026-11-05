// WinHTTP transport for Windows: part of the OS, so no libcurl/OpenSSL to ship.
// Same rules as the curl transport: no proxy, no automatic redirects, and
// HTTP Digest only after the server challenges for it.
#include "internal.hpp"

#if defined(_WIN32)

#include <windows.h>
#include <winhttp.h>

namespace ptz {

std::wstring widen(const std::string &text) {
    if (text.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string narrow(const std::wstring &text) {
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::string windows_error(unsigned long code) {
    wchar_t *buffer = nullptr;
    HMODULE winhttp = GetModuleHandleW(L"winhttp.dll");
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM |
                  (winhttp ? FORMAT_MESSAGE_FROM_HMODULE : 0);
    DWORD n = FormatMessageW(flags, winhttp, code, 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
    std::string text = n ? trim(narrow(std::wstring(buffer, n))) : "error " + std::to_string(code);
    if (buffer) LocalFree(buffer);
    return text;
}

namespace {

struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET handle) : h(handle) {}
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
    explicit operator bool() const { return h != nullptr; }
};

std::string friendly(DWORD code) {
    switch (code) {
        case ERROR_WINHTTP_TIMEOUT: return "The operation timed out";
        case ERROR_WINHTTP_CANNOT_CONNECT: return "Could not connect (refused or unreachable)";
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "Could not resolve host name";
        case ERROR_WINHTTP_CONNECTION_ERROR: return "Connection reset by the camera";
        case ERROR_WINHTTP_SECURE_FAILURE: return "TLS certificate could not be verified";
        default: return windows_error(code);
    }
}

}  // namespace

void http_global_init() {}

HttpResponse http_perform(const HttpRequest &request) {
    HttpResponse response;
    auto fail = [&](DWORD code) {
        response.transport_ok = false;
        response.tls_verify_failed = code == ERROR_WINHTTP_SECURE_FAILURE;
        response.transport_error = friendly(code);
        return response;
    };

    std::wstring url = widen(request.url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    parts.dwSchemeLength = 1;
    parts.dwExtraInfoLength = 1;  // query string, returned as a pointer into `url`
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) return fail(GetLastError());
    std::wstring object(path, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo) object.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (object.empty()) object = L"/";
    std::wstring host_name(host, parts.dwHostNameLength);
    if (host_name.size() > 2 && host_name.front() == L'[') host_name = host_name.substr(1, host_name.size() - 2);
    bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

    Handle session(WinHttpOpen(L"PTZControl/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return fail(GetLastError());
    int timeout_ms = static_cast<int>(request.timeout * 1000);
    WinHttpSetTimeouts(session.h, timeout_ms, std::min(timeout_ms, 10000), timeout_ms, timeout_ms);

    Handle connection(WinHttpConnect(session.h, host_name.c_str(), parts.nPort, 0));
    if (!connection) return fail(GetLastError());
    std::wstring method = widen(request.method);
    Handle req(WinHttpOpenRequest(connection.h, method.c_str(), object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
    if (!req) return fail(GetLastError());
    DWORD option = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(req.h, WINHTTP_OPTION_DISABLE_FEATURE, &option, sizeof option);
    // Never answer a challenge with the user's Windows logon (NTLM/Negotiate).
    DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    WinHttpSetOption(req.h, WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof logon);

    std::wstring headers;
    for (const auto &h : request.headers) headers += widen(h) + L"\r\n";
    std::wstring user = widen(request.user), password = widen(request.password);

    for (int attempt = 0; attempt < 2; ++attempt) {
        BOOL sent = WinHttpSendRequest(req.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                       headers.empty() ? 0 : static_cast<DWORD>(-1L),
                                       request.body.empty() ? WINHTTP_NO_REQUEST_DATA
                                                            : const_cast<char *>(request.body.data()),
                                       static_cast<DWORD>(request.body.size()),
                                       static_cast<DWORD>(request.body.size()), 0);
        if (!sent || !WinHttpReceiveResponse(req.h, nullptr)) return fail(GetLastError());
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        response.status = static_cast<long>(status);
        if (status == 401 && request.digest_auth && attempt == 0) {
            DWORD supported = 0, first = 0, target = 0;
            if (WinHttpQueryAuthSchemes(req.h, &supported, &first, &target) && (supported & WINHTTP_AUTH_SCHEME_DIGEST) &&
                WinHttpSetCredentials(req.h, target, WINHTTP_AUTH_SCHEME_DIGEST, user.c_str(), password.c_str(), nullptr)) {
                // Drain the challenge body, then resend with Digest.
                char discard[4096];
                DWORD got = 0;
                while (WinHttpReadData(req.h, discard, sizeof discard, &got) && got) {}
                continue;
            }
        }
        break;
    }

    DWORD raw_size = 0;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &raw_size,
                        WINHTTP_NO_HEADER_INDEX);
    if (raw_size) {
        std::wstring raw(raw_size / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(),
                                &raw_size, WINHTTP_NO_HEADER_INDEX)) {
            for (const auto &line : log_lines(narrow(raw))) {
                auto colon = line.find(':');
                if (colon != std::string::npos)
                    response.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
            }
        }
    }

    char buffer[8192];
    while (response.body.size() < request.max_body) {
        DWORD got = 0;
        if (!WinHttpReadData(req.h, buffer, sizeof buffer, &got)) {
            DWORD code = GetLastError();
            // A cut-off body still has a valid status line; probes only need that.
            if (response.body.empty() && request.max_body > 1) return fail(code);
            break;
        }
        if (!got) break;
        response.body.append(buffer, std::min<size_t>(got, request.max_body - response.body.size()));
    }
    response.transport_ok = true;
    return response;
}

}  // namespace ptz

#endif
