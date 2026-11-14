// SSH tunnel for Windows using the OpenSSH Client that ships with Windows
// (System32\OpenSSH\ssh.exe). Windows OpenSSH has no connection multiplexing,
// so readiness comes from ssh's own "Local forwarding listening" log line and
// the camera HTTPS port and RTSP video port are forwarded up front. Prompts (password, passphrase,
// MFA) reach the host UI through SSH_ASKPASS: ssh runs the host executable,
// which relays over a named pipe that only the current user may open.
#include "internal.hpp"

#if defined(_WIN32)

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <sddl.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

namespace ptz {

namespace {

using Clock = std::chrono::steady_clock;
constexpr const char *kPipeVariable = "PTZ_ASKPASS_SOCKET";

void ensure_winsock() {
    static bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!started) throw Error("Cannot initialise Windows networking");
}

int reserve_port() {
    ensure_winsock();
    SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) throw Error("Cannot reserve a local port");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int size = sizeof address;
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), size) != 0 ||
        getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size) != 0) {
        closesocket(fd);
        throw Error("Cannot reserve a local port");
    }
    closesocket(fd);
    return ntohs(address.sin_port);
}

std::wstring system_directory() {
    wchar_t buffer[MAX_PATH];
    UINT n = GetSystemDirectoryW(buffer, MAX_PATH);
    return std::wstring(buffer, n);
}

std::string find_tool(const char *name) {
    // The inbox OpenSSH Client first; then anything on PATH (e.g. Git for Windows).
    std::wstring inbox = system_directory() + L"\\OpenSSH\\" + widen(name) + L".exe";
    if (GetFileAttributesW(inbox.c_str()) != INVALID_FILE_ATTRIBUTES) return narrow(inbox);
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, widen(name).c_str(), L".exe", MAX_PATH, found, nullptr)) return narrow(found);
    throw Error(std::string(name) +
                ".exe was not found. Install Windows' OpenSSH Client (Settings > System > Optional features), "
                "or use the Local route");
}

std::wstring quote(const std::wstring &arg) {
    // CommandLineToArgvW rules: backslashes are literal unless they precede a quote.
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        slashes = 0;
        out.push_back(c);
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

struct Process {
    HANDLE process = nullptr;
    HANDLE job = nullptr;

    void close() {
        if (process) {
            if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
                TerminateProcess(process, 1);
                WaitForSingleObject(process, 5000);
            }
            CloseHandle(process);
            process = nullptr;
        }
        if (job) {
            CloseHandle(job);  // KILL_ON_JOB_CLOSE ends anything ssh started
            job = nullptr;
        }
    }

    bool running() const { return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT; }
};

Process spawn(const std::vector<std::string> &argv, const std::vector<std::string> &extra_env = {}) {
    std::wstring command;
    for (const auto &a : argv) command += (command.empty() ? L"" : L" ") + quote(widen(a));

    // Environment block: ours plus overrides, sorted by the CRT is not required.
    std::wstring environment;
    wchar_t *block = GetEnvironmentStringsW();
    for (wchar_t *e = block; *e; e += wcslen(e) + 1) {
        std::wstring entry(e);
        bool replaced = false;
        for (const auto &extra : extra_env) {
            std::wstring name = widen(extra.substr(0, extra.find('=') + 1));
            if (_wcsnicmp(entry.c_str(), name.c_str(), name.size()) == 0) replaced = true;
        }
        if (!replaced) environment += entry + L'\0';
    }
    FreeEnvironmentStringsW(block);
    for (const auto &extra : extra_env) environment += widen(extra) + L'\0';
    environment += L'\0';

    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    HANDLE nul_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    HANDLE nul_out = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);

    // Inherit only the NUL handles, nothing else this process has open.
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buffer(attr_size);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buffer.data());
    InitializeProcThreadAttributeList(attributes, 1, 0, &attr_size);
    HANDLE inherited[2] = {nul_in, nul_out};
    UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof inherited, nullptr, nullptr);

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nul_in;
    startup.StartupInfo.hStdOutput = nul_out;
    startup.StartupInfo.hStdError = nul_out;
    startup.lpAttributeList = attributes;

    Process result;
    result.job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (result.job) SetInformationJobObject(result.job, JobObjectExtendedLimitInformation, &limits, sizeof limits);

    PROCESS_INFORMATION info{};
    std::wstring mutable_command = command;
    BOOL ok = CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT |
                                 CREATE_SUSPENDED,
                             environment.data(), nullptr, &startup.StartupInfo, &info);
    DWORD error = GetLastError();
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(nul_in);
    CloseHandle(nul_out);
    if (!ok) {
        if (result.job) CloseHandle(result.job);
        throw Error("Cannot start " + argv[0] + ": " + windows_error(error));
    }
    if (result.job) AssignProcessToJobObject(result.job, info.hProcess);
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    result.process = info.hProcess;
    return result;
}

int run(const std::vector<std::string> &argv, DWORD timeout_ms = 5000) {
    Process p = spawn(argv);
    DWORD code = 1;
    if (WaitForSingleObject(p.process, timeout_ms) == WAIT_OBJECT_0) GetExitCodeProcess(p.process, &code);
    else code = static_cast<DWORD>(-1);
    p.close();
    return static_cast<int>(code);
}

std::wstring current_user_sid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<char> buffer(size);
    std::wstring result;
    if (GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        wchar_t *text = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &text)) {
            result = text;
            LocalFree(text);
        }
    }
    CloseHandle(token);
    return result;
}

// Blocking I/O on an overlapped pipe, bounded by a stop event and a timeout.
bool pipe_io(HANDLE pipe, HANDLE stop, bool write, void *data, DWORD size, DWORD *done, DWORD timeout_ms) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BOOL ok = write ? WriteFile(pipe, data, size, nullptr, &overlapped) : ReadFile(pipe, data, size, nullptr, &overlapped);
    bool success = false;
    if (ok || GetLastError() == ERROR_IO_PENDING || GetLastError() == ERROR_MORE_DATA) {
        HANDLE events[2] = {overlapped.hEvent, stop};
        DWORD wait = WaitForMultipleObjects(2, events, FALSE, timeout_ms);
        if (wait == WAIT_OBJECT_0) {
            success = GetOverlappedResult(pipe, &overlapped, done, FALSE) || GetLastError() == ERROR_MORE_DATA;
        } else {
            CancelIoEx(pipe, &overlapped);
            GetOverlappedResult(pipe, &overlapped, done, TRUE);
        }
    }
    CloseHandle(overlapped.hEvent);
    return success;
}

class AskpassServer {
public:
    using Handler = std::function<std::optional<std::string>(char kind, const std::string &prompt)>;

    AskpassServer(std::wstring name, Handler handler) : name_(std::move(name)), handler_(std::move(handler)) {
        std::wstring sid = current_user_sid();
        if (sid.empty()) throw Error("Cannot determine the current Windows user");
        // Protected DACL: full access for this user only; no Everyone/anonymous read.
        std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr))
            throw Error("Cannot secure the SSH prompt relay");
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        create_instance(true);
        thread_ = std::thread([this] { loop(); });
    }

    ~AskpassServer() {
        SetEvent(stop_);
        if (thread_.joinable()) thread_.join();
        if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
        CloseHandle(stop_);
        LocalFree(descriptor_);
    }

    bool busy() const { return busy_; }

private:
    void create_instance(bool first) {
        SECURITY_ATTRIBUTES security{sizeof security, descriptor_, FALSE};
        pipe_ = CreateNamedPipeW(name_.c_str(),
                                 PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                 PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                 PIPE_UNLIMITED_INSTANCES, 8192, 8192, 0, &security);
        if (pipe_ == INVALID_HANDLE_VALUE) throw Error("Cannot create the SSH prompt relay");
    }

    void loop() {
        while (WaitForSingleObject(stop_, 0) == WAIT_TIMEOUT) {
            OVERLAPPED overlapped{};
            overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            bool connected = ConnectNamedPipe(pipe_, &overlapped) != 0 || GetLastError() == ERROR_PIPE_CONNECTED;
            if (!connected && GetLastError() == ERROR_IO_PENDING) {
                HANDLE events[2] = {overlapped.hEvent, stop_};
                if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0) {
                    DWORD ignored;
                    connected = GetOverlappedResult(pipe_, &overlapped, &ignored, FALSE) != 0;
                } else {
                    CancelIoEx(pipe_, &overlapped);
                    DWORD ignored;
                    GetOverlappedResult(pipe_, &overlapped, &ignored, TRUE);
                }
            }
            CloseHandle(overlapped.hEvent);
            if (!connected) {
                if (WaitForSingleObject(stop_, 0) != WAIT_TIMEOUT) return;
                DisconnectNamedPipe(pipe_);
                continue;
            }
            serve();
            FlushFileBuffers(pipe_);
            DisconnectNamedPipe(pipe_);
        }
    }

    void serve() {
        char buffer[8192];
        DWORD got = 0;
        if (!pipe_io(pipe_, stop_, false, buffer, sizeof buffer, &got, 5000) || got == 0) return;
        std::string request(buffer, got);
        busy_ = true;
        std::optional<std::string> answer;
        try {
            answer = handler_(request[0], printable(request.substr(1)));
        } catch (...) {
        }
        busy_ = false;
        std::string reply = answer ? "1" + *answer : std::string("0");
        DWORD sent = 0;
        pipe_io(pipe_, stop_, true, reply.data(), static_cast<DWORD>(reply.size()), &sent, 5000);
        SecureZeroMemory(reply.data(), reply.size());
    }

    std::wstring name_;
    Handler handler_;
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE stop_ = nullptr;
    std::atomic<bool> busy_{false};
    std::thread thread_;
};

std::string read_shared(const std::wstring &path) {
    // ssh keeps the log open for writing; share everything while reading it.
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::string text;
    char buffer[8192];
    DWORD got = 0;
    while (ReadFile(file, buffer, sizeof buffer, &got, nullptr) && got) text.append(buffer, got);
    CloseHandle(file);
    return text;
}

bool port_accepts(int port) {
    SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<u_short>(port));
    bool ok = connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0;
    closesocket(fd);
    return ok;
}

class WindowsTunnel final : public Tunnel {
public:
    WindowsTunnel(TunnelParams params, const Hooks &hooks) : p_(std::move(params)), hooks_(hooks) {
        ensure_winsock();
        ssh_ = find_tool("ssh");
        if (p_.askpass_path.empty()) throw Error("Host did not provide an SSH prompt helper");
        target_ = bracket_host(p_.camera_host);

        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        std::string tag;
        for (unsigned char b : random_bytes(8)) {
            static const char hex[] = "0123456789abcdef";
            tag += hex[b >> 4];
            tag += hex[b & 15];
        }
        log_ = std::wstring(temp) + L"ptz-" + widen(tag) + L".log";
        pipe_ = L"\\\\.\\pipe\\ptz-askpass-" + widen(tag);
        try {
            askpass_ = std::make_unique<AskpassServer>(pipe_, [this](char kind, const std::string &prompt) {
                return answer(kind, prompt);
            });
            local_port_ = reserve_port();
            https_local_port_ = reserve_port();
            rtsp_local_port_ = reserve_port();
            for (int attempt = 0;; ++attempt) {
                launch();
                std::string failure = wait_ready();
                if (failure.empty()) break;
                if (attempt == 0 && replace_changed_host_key(
                                        last_log_, p_.ssh_host, p_.ssh_port, hooks_, [] { return find_tool("ssh-keygen"); },
                                        [](const std::vector<std::string> &argv) { return run(argv); })) {
                    hooks_.log("Retrying the SSH connection with the new host key");
                    continue;
                }
                if (!fallback_ && ssh_integrity_failure(last_log_)) {
                    fallback_ = true;
                    hooks_.log("SSH packet integrity check failed; retrying with AES-CTR and HMAC-SHA2");
                    continue;
                }
                throw Error(failure);
            }
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~WindowsTunnel() override { cleanup(); }

    int local_port() const override { return local_port_; }

    bool alive() override { return ssh_process_.running(); }

    int forward(int camera_port) override {
        if (camera_port == p_.https_port) return https_local_port_;
        if (camera_port == p_.rtsp_port) return rtsp_local_port_;
        throw Error("Windows can't forward the camera's port " + std::to_string(camera_port) +
                    " through SSH: only the HTTPS port set under Advanced (" + std::to_string(p_.https_port) +
                    ") and the video port " + std::to_string(p_.rtsp_port) + " are forwarded");
    }

private:
    std::optional<std::string> answer(char kind, const std::string &prompt) {
        if (kind == 'N') {
            if (hooks_.log) hooks_.log("SSH: " + prompt);
            return std::string();
        }
        if (kind == 'C') {
            if (hooks_.confirm && hooks_.confirm("SSH confirmation", prompt)) return std::string("yes");
            return std::nullopt;
        }
        std::string l = lower(prompt);
        if (!password_used_ && !p_.ssh_password.empty() && l.find("password") != std::string::npos &&
            l.find("passphrase") == std::string::npos) {
            password_used_ = true;
            if (hooks_.log) hooks_.log("Answering the SSH password prompt with the Host Password");
            return p_.ssh_password;
        }
        if (!hooks_.prompt) return std::nullopt;
        return hooks_.prompt(prompt, true);
    }

    void launch() {
        DeleteFileW(log_.c_str());
        password_used_ = false;
        int connect_timeout = std::max(5, static_cast<int>(p_.timeout));
        std::vector<std::string> command = {
            ssh_, "-v", "-E", narrow(log_), "-N", "-T",
            "-o", "StrictHostKeyChecking=accept-new",
            "-o", "ExitOnForwardFailure=yes", "-o", "PermitLocalCommand=no",
            "-o", "BatchMode=no",
            "-o", "ConnectTimeout=" + std::to_string(connect_timeout),
            "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3",
            "-L", "127.0.0.1:" + std::to_string(local_port_) + ":" + target_ + ":" + std::to_string(p_.camera_port),
            "-L", "127.0.0.1:" + std::to_string(https_local_port_) + ":" + target_ + ":" + std::to_string(p_.https_port),
            "-L", "127.0.0.1:" + std::to_string(rtsp_local_port_) + ":" + target_ + ":" + std::to_string(p_.rtsp_port),
            "-p", std::to_string(p_.ssh_port), "-l", p_.ssh_user,
        };
        if (fallback_) {
            auto extra = ssh_fallback_options();
            command.insert(command.end(), extra.begin(), extra.end());
        }
        if (!p_.ssh_identity.empty()) {
            if (GetFileAttributesW(widen(p_.ssh_identity).c_str()) == INVALID_FILE_ATTRIBUTES)
                throw Error("SSH key file is not readable: " + p_.ssh_identity);
            command.insert(command.end(), {"-i", p_.ssh_identity});
        }
        command.insert(command.end(), {"--", p_.ssh_host});
        ssh_process_.close();
        ssh_process_ = spawn(command, {"SSH_ASKPASS=" + p_.askpass_path, "SSH_ASKPASS_REQUIRE=force",
                                       std::string(kPipeVariable) + "=" + narrow(pipe_), "DISPLAY=:0"});
        if (hooks_.log)
            hooks_.log("Opening SSH tunnel to " + p_.ssh_user + "@" + p_.ssh_host + ":" + std::to_string(p_.ssh_port));
    }

    std::string wait_ready() {
        auto budget = std::chrono::duration<double>(std::max(10.0, p_.timeout) + 20);
        auto deadline = Clock::now() + budget;
        std::string listening = "Local forwarding listening on 127.0.0.1 port " + std::to_string(local_port_);
        auto next_probe = Clock::now() + std::chrono::seconds(2);
        while (true) {
            if (hooks_.cancelled()) {
                ssh_process_.close();
                throw Error("Connection cancelled");
            }
            if (!ssh_process_.running()) {
                last_log_ = read_shared(log_);
                ssh_process_.close();
                return ssh_failure_message(last_log_);
            }
            if (read_shared(log_).find(listening) != std::string::npos) return "";
            // Fallback when the log is not readable: the listener itself answers.
            if (Clock::now() > next_probe && !askpass_->busy()) {
                if (port_accepts(local_port_)) return "";
                next_probe = Clock::now() + std::chrono::seconds(2);
            }
            if (askpass_->busy()) deadline = Clock::now() + budget;  // the operator is typing
            if (Clock::now() > deadline) {
                ssh_process_.close();
                return "SSH connection timed out; check the host, port and network access";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void cleanup() {
        ssh_process_.close();
        askpass_.reset();
        if (!log_.empty()) DeleteFileW(log_.c_str());
    }

    TunnelParams p_;
    const Hooks &hooks_;
    std::string ssh_, target_, last_log_;
    std::wstring log_, pipe_;
    int local_port_ = 0, https_local_port_ = 0, rtsp_local_port_ = 0;
    bool password_used_ = false, fallback_ = false;
    Process ssh_process_;
    std::unique_ptr<AskpassServer> askpass_;
};

}  // namespace

std::unique_ptr<Tunnel> open_tunnel(const TunnelParams &params, const Hooks &hooks) {
    return std::make_unique<WindowsTunnel>(params, hooks);
}

}  // namespace ptz

extern "C" int ptz_is_askpass_invocation(void) { return GetEnvironmentVariableW(L"PTZ_ASKPASS_SOCKET", nullptr, 0) > 0; }

extern "C" int ptz_askpass_main(int, char **) {
    wchar_t name[256];
    if (!GetEnvironmentVariableW(L"PTZ_ASKPASS_SOCKET", name, 256)) return 2;
    wchar_t mode[32] = {};
    GetEnvironmentVariableW(L"SSH_ASKPASS_PROMPT", mode, 32);
    char kind = _wcsicmp(mode, L"confirm") == 0 ? 'C' : _wcsicmp(mode, L"none") == 0 ? 'N' : 'P';
    // The prompt is argv[1]; read it as UTF-16 so non-ASCII prompts survive.
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::string request(1, kind);
    if (argv && argc > 1) request += ptz::narrow(argv[1]);
    if (argv) LocalFree(argv);

    if (!WaitNamedPipeW(name, 5000)) return 1;
    HANDLE pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return 1;
    DWORD message_mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &message_mode, nullptr, nullptr);
    DWORD done = 0;
    std::string reply;
    if (WriteFile(pipe, request.data(), static_cast<DWORD>(request.size()), &done, nullptr)) {
        char buffer[8192];
        // The server answers only after the operator has, so this read may take a while.
        if (ReadFile(pipe, buffer, sizeof buffer, &done, nullptr)) reply.assign(buffer, done);
    }
    CloseHandle(pipe);
    if (reply.empty() || reply[0] != '1') return 1;
    std::string answer = reply.substr(1) + "\n";
    SecureZeroMemory(reply.data(), reply.size());
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), answer.data(), static_cast<DWORD>(answer.size()), &done, nullptr);
    SecureZeroMemory(answer.data(), answer.size());
    return 0;
}

#endif
