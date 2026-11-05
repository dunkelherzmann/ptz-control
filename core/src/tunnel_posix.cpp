// SSH tunnel for macOS/Linux using the system OpenSSH client. Prompts from ssh
// (password, key passphrase, MFA) reach the host UI through an askpass relay:
// ssh runs the host executable as SSH_ASKPASS, which connects back to a Unix
// socket in a private directory and asks this process for the answer.
#include "internal.hpp"

#if !defined(_WIN32)

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

extern char **environ;

namespace ptz {

namespace {

using Clock = std::chrono::steady_clock;
constexpr const char *kSocketVariable = "PTZ_ASKPASS_SOCKET";

int reserve_port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw Error("Cannot reserve a local port");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof address;
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), size) != 0 ||
        getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size) != 0) {
        close(fd);
        throw Error("Cannot reserve a local port");
    }
    close(fd);
    return ntohs(address.sin_port);
}

std::string find_tool(const char *name) {
    for (const char *dir : {"/usr/bin", "/bin", "/usr/local/bin", "/opt/homebrew/bin"}) {
        std::string path = std::string(dir) + "/" + name;
        if (access(path.c_str(), X_OK) == 0) return path;
    }
    throw Error(std::string(name) + " was not found; install the OpenSSH client");
}

pid_t spawn(const std::vector<std::string> &argv, const std::vector<std::string> &extra_env = {}) {
    std::vector<char *> args;
    for (auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);

    std::vector<std::string> env_storage;
    for (char **e = environ; *e; ++e) {
        std::string entry(*e);
        bool replaced = false;
        for (auto &extra : extra_env)
            if (entry.compare(0, extra.find('=') + 1, extra, 0, extra.find('=') + 1) == 0) replaced = true;
        if (!replaced) env_storage.push_back(entry);
    }
    for (auto &extra : extra_env) env_storage.push_back(extra);
    std::vector<char *> envp;
    for (auto &e : env_storage) envp.push_back(const_cast<char *>(e.c_str()));
    envp.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    for (int fd = 0; fd <= 2; ++fd) posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd ? O_WRONLY : O_RDONLY, 0);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    short flags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
#if defined(__APPLE__)
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;  // no descriptor of ours leaks into ssh
#endif
    posix_spawnattr_setflags(&attributes, flags);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attributes, &none);
    posix_spawnattr_setsigdefault(&attributes, &all);
    pid_t pid = -1;
    int rc = posix_spawn(&pid, args[0], &actions, &attributes, args.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (rc != 0) throw Error(std::string("Cannot start ") + argv[0] + ": " + std::strerror(rc));
    return pid;
}

void reap(pid_t pid, double grace_seconds) {
    auto deadline = Clock::now() + std::chrono::duration<double>(grace_seconds);
    int status = 0;
    while (waitpid(pid, &status, WNOHANG) == 0) {
        if (Clock::now() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

int run(const std::vector<std::string> &argv, double timeout_seconds = 5) {
    pid_t pid = spawn(argv);
    auto deadline = Clock::now() + std::chrono::duration<double>(timeout_seconds);
    int status = 0;
    while (true) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (done < 0) return -1;
        if (Clock::now() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool peer_is_same_user(int fd) {
#if defined(__APPLE__) || defined(__FreeBSD__)
    uid_t uid;
    gid_t gid;
    return getpeereid(fd, &uid, &gid) == 0 && uid == getuid();
#else
    struct ucred credentials{};
    socklen_t size = sizeof credentials;
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) == 0 && credentials.uid == getuid();
#endif
}

bool read_all(int fd, std::string &out, size_t limit) {
    char buffer[1024];
    while (out.size() < limit) {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 5000) <= 0) return false;
        ssize_t n = read(fd, buffer, sizeof buffer);
        if (n < 0) return false;
        if (n == 0) return true;
        out.append(buffer, static_cast<size_t>(n));
    }
    return true;
}

void write_all(int fd, const std::string &data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = write(fd, data.data() + sent, data.size() - sent);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

// One question at a time: ssh blocks on each prompt anyway.
class AskpassServer {
public:
    using Handler = std::function<std::optional<std::string>(char kind, const std::string &prompt)>;

    AskpassServer(const std::string &path, Handler handler) : path_(path), handler_(std::move(handler)) {
        fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (fd_ < 0 || path.size() >= sizeof address.sun_path) throw Error("Cannot create the SSH prompt relay");
        std::strncpy(address.sun_path, path.c_str(), sizeof address.sun_path - 1);
        fcntl(fd_, F_SETFD, FD_CLOEXEC);
        if (bind(fd_, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0 || listen(fd_, 4) != 0) {
            close(fd_);
            throw Error("Cannot create the SSH prompt relay");
        }
        thread_ = std::thread([this] { loop(); });
    }

    ~AskpassServer() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        close(fd_);
        unlink(path_.c_str());
    }

    bool busy() const { return busy_; }

private:
    void loop() {
        while (!stop_) {
            pollfd p{fd_, POLLIN, 0};
            if (poll(&p, 1, 100) <= 0) continue;
            int client = accept(fd_, nullptr, nullptr);
            if (client < 0) continue;
            if (peer_is_same_user(client)) {
                std::string request;
                if (read_all(client, request, 8192) && !request.empty()) {
                    busy_ = true;
                    std::optional<std::string> answer;
                    try {
                        answer = handler_(request[0], printable(request.substr(1)));
                    } catch (...) {
                    }
                    busy_ = false;
                    write_all(client, answer ? "1" + *answer : std::string("0"));
                }
            }
            close(client);
        }
    }

    std::string path_;
    Handler handler_;
    int fd_ = -1;
    std::atomic<bool> stop_{false}, busy_{false};
    std::thread thread_;
};

std::string read_file(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

class PosixTunnel final : public Tunnel {
public:
    PosixTunnel(TunnelParams params, const Hooks &hooks) : p_(std::move(params)), hooks_(hooks) {
        ssh_ = find_tool("ssh");
        if (p_.askpass_path.empty()) throw Error("Host did not provide an SSH prompt helper");
        char pattern[] = "/tmp/ptz-XXXXXX";  // short path: Unix socket names are limited
        if (!mkdtemp(pattern)) throw Error("Cannot create a private temporary directory");
        dir_ = pattern;
        chmod(dir_.c_str(), 0700);
        control_ = dir_ + "/c";
        log_ = dir_ + "/log";
        target_ = bracket_host(p_.camera_host);
        try {
            askpass_ = std::make_unique<AskpassServer>(dir_ + "/a", [this](char kind, const std::string &prompt) {
                return answer(kind, prompt);
            });
            local_port_ = reserve_port();
            for (int attempt = 0;; ++attempt) {
                launch();
                std::string failure = wait_ready();
                if (failure.empty()) break;
                if (attempt == 0 && replace_changed_host_key()) {
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

    ~PosixTunnel() override { cleanup(); }

    int local_port() const override { return local_port_; }

    bool alive() override {
        if (pid_ <= 0) return false;
        int status;
        if (waitpid(pid_, &status, WNOHANG) == pid_) {
            pid_ = -1;
            return false;
        }
        return true;
    }

    int forward(int camera_port) override {
        int port = reserve_port();
        int rc = run({ssh_, "-S", control_, "-O", "forward", "-o", "BatchMode=yes", "-o", "ExitOnForwardFailure=yes",
                      "-L", "127.0.0.1:" + std::to_string(port) + ":" + target_ + ":" + std::to_string(camera_port),
                      "-p", std::to_string(p_.ssh_port), "-l", p_.ssh_user, "--", p_.ssh_host});
        if (rc != 0) throw Error("Could not forward the camera HTTPS port through the existing SSH connection");
        return port;
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
        unlink(control_.c_str());
        { std::ofstream(log_, std::ios::trunc); }
        chmod(log_.c_str(), 0600);
        password_used_ = false;
        int connect_timeout = std::max(5, static_cast<int>(p_.timeout));
        std::vector<std::string> command = {
            ssh_, "-E", log_, "-N", "-T", "-M", "-S", control_,
            "-o", "StrictHostKeyChecking=accept-new",
            "-o", "ControlPersist=no", "-o", "ForkAfterAuthentication=no",
            "-o", "ExitOnForwardFailure=yes", "-o", "PermitLocalCommand=no",
            "-o", "RemoteCommand=none", "-o", "BatchMode=no",
            "-o", "ConnectTimeout=" + std::to_string(connect_timeout),
            "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3",
            "-L", "127.0.0.1:" + std::to_string(local_port_) + ":" + target_ + ":" + std::to_string(p_.camera_port),
            "-p", std::to_string(p_.ssh_port), "-l", p_.ssh_user,
        };
        if (fallback_) {
            auto extra = ssh_fallback_options();
            command.insert(command.end(), extra.begin(), extra.end());
        }
        if (!p_.ssh_identity.empty()) {
            if (access(p_.ssh_identity.c_str(), R_OK) != 0) throw Error("SSH key file is not readable: " + p_.ssh_identity);
            command.insert(command.end(), {"-i", p_.ssh_identity});
        }
        command.insert(command.end(), {"--", p_.ssh_host});
        pid_ = spawn(command, {"SSH_ASKPASS=" + p_.askpass_path, "SSH_ASKPASS_REQUIRE=force",
                               std::string(kSocketVariable) + "=" + dir_ + "/a", "DISPLAY=:0"});
        if (hooks_.log)
            hooks_.log("Opening SSH tunnel to " + p_.ssh_user + "@" + p_.ssh_host + ":" + std::to_string(p_.ssh_port));
    }

    // Returns an empty string once the master connection is ready.
    std::string wait_ready() {
        auto budget = std::chrono::duration<double>(std::max(10.0, p_.timeout) + 20);
        auto deadline = Clock::now() + budget;
        while (true) {
            if (hooks_.cancelled()) {
                stop_ssh();
                throw Error("Connection cancelled");
            }
            int status = 0;
            if (waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                last_log_ = read_file(log_);
                return ssh_failure_message(last_log_);
            }
            if (access(control_.c_str(), F_OK) == 0 &&
                run({ssh_, "-S", control_, "-O", "check", "-p", std::to_string(p_.ssh_port), "-l", p_.ssh_user, "--",
                     p_.ssh_host}) == 0)
                return "";
            if (askpass_->busy()) deadline = Clock::now() + budget;  // the operator is typing
            if (Clock::now() > deadline) {
                stop_ssh();
                return "SSH connection timed out; check the host, port and network access";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    bool replace_changed_host_key() {
        return ptz::replace_changed_host_key(
            last_log_, p_.ssh_host, p_.ssh_port, hooks_, [] { return find_tool("ssh-keygen"); },
            [](const std::vector<std::string> &argv) { return run(argv); });
    }

    void stop_ssh() {
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            reap(pid_, 5);
            pid_ = -1;
        }
    }

    void cleanup() {
        stop_ssh();
        askpass_.reset();
        if (!dir_.empty()) {
            unlink(control_.c_str());
            unlink(log_.c_str());
            rmdir(dir_.c_str());
            dir_.clear();
        }
    }

    TunnelParams p_;
    const Hooks &hooks_;
    std::string ssh_, dir_, control_, log_, target_, last_log_;
    int local_port_ = 0;
    pid_t pid_ = -1;
    bool password_used_ = false, fallback_ = false;
    std::unique_ptr<AskpassServer> askpass_;
};

}  // namespace

std::unique_ptr<Tunnel> open_tunnel(const TunnelParams &params, const Hooks &hooks) {
    return std::make_unique<PosixTunnel>(params, hooks);
}

}  // namespace ptz

extern "C" int ptz_is_askpass_invocation(void) { return getenv("PTZ_ASKPASS_SOCKET") != nullptr; }

extern "C" int ptz_askpass_main(int argc, char **argv) {
    const char *path = getenv("PTZ_ASKPASS_SOCKET");
    if (!path) return 2;
    const char *mode = getenv("SSH_ASKPASS_PROMPT");
    char kind = mode && std::strcmp(mode, "confirm") == 0 ? 'C' : mode && std::strcmp(mode, "none") == 0 ? 'N' : 'P';
    std::string request(1, kind);
    if (argc > 1) request += argv[1];

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (fd < 0 || std::strlen(path) >= sizeof address.sun_path) return 1;
    std::strncpy(address.sun_path, path, sizeof address.sun_path - 1);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0) return 1;
    ptz::write_all(fd, request);
    shutdown(fd, SHUT_WR);
    std::string reply;
    char buffer[1024];
    ssize_t n;
    while ((n = read(fd, buffer, sizeof buffer)) > 0) reply.append(buffer, static_cast<size_t>(n));
    close(fd);
    if (reply.empty() || reply[0] != '1') return 1;
    std::string answer = reply.substr(1) + "\n";
    ssize_t ignored = write(STDOUT_FILENO, answer.data(), answer.size());
    (void)ignored;
    return 0;
}

#endif
