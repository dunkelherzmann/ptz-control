// SSH log interpretation shared by the POSIX and Windows tunnels.
#include "internal.hpp"

#include <algorithm>
#include <regex>
#include <sstream>

namespace ptz {

std::vector<std::string> log_lines(const std::string &text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

std::string ssh_failure_message(const std::string &log) {
    static const char *known[] = {
        "Permission denied", "Could not resolve hostname", "Connection refused", "Connection timed out",
        "Operation timed out", "No route to host", "Host key verification failed", "Too many authentication",
        "REMOTE HOST IDENTIFICATION HAS CHANGED", "forwarding failed", "Network is unreachable",
        "Connection closed", "kex_exchange_identification", "Bad owner or permissions",
    };
    auto lines = log_lines(log);
    for (const char *pattern : known)
        for (const auto &line : lines)
            if (line.find(pattern) != std::string::npos) return "SSH: " + printable(trim(line));
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        std::string line = trim(*it);
        // Skip banners and the verbose/debug chatter of a -v log.
        if (!line.empty() && line[0] != '@' && line.rfind("debug", 0) != 0) return "SSH: " + printable(line);
    }
    return "SSH connection/authentication failed; verify host key, credentials and network access";
}

bool ssh_integrity_failure(const std::string &log) {
    return log.find("message authentication code incorrect") != std::string::npos ||
           log.find("Corrupted MAC on input") != std::string::npos;
}

std::vector<std::string> ssh_fallback_options() {
    // AES-CTR with HMAC-SHA2: universally supported, and avoids chacha20-poly1305, AES-GCM and UMAC.
    return {"-o", "Ciphers=aes256-ctr,aes192-ctr,aes128-ctr",
            "-o", "MACs=hmac-sha2-256-etm@openssh.com,hmac-sha2-512-etm@openssh.com,hmac-sha2-256,hmac-sha2-512"};
}

bool replace_changed_host_key(const std::string &log, const std::string &host, int port, const Hooks &hooks,
                              const std::function<std::string()> &find_keygen,
                              const std::function<int(const std::vector<std::string> &)> &run) {
    if (log.find("REMOTE HOST IDENTIFICATION HAS CHANGED") == std::string::npos) return false;
    std::regex offending(R"(^Offending (?:\S+ )?key(?: for IP)? in (.+):(\d+)\s*$)");
    std::regex lookup(R"(^Host key for (.+) has changed and you have requested strict checking\.\s*$)");
    std::regex fingerprint(R"((SHA256:[A-Za-z0-9+/=]+))");
    std::vector<std::string> files;
    std::string identifier, print;
    std::smatch match;
    for (const auto &line : log_lines(log)) {
        if (std::regex_search(line, match, offending)) {
            if (std::find(files.begin(), files.end(), match[1].str()) == files.end()) files.push_back(match[1]);
        } else if (std::regex_search(line, match, lookup)) {
            identifier = match[1];
        } else if (print.empty() && std::regex_search(line, match, fingerprint)) {
            print = match[1];
        }
    }
    if (files.empty()) return false;
    if (identifier.empty()) identifier = port == 22 ? host : "[" + host + "]:" + std::to_string(port);
    for (unsigned char c : identifier)
        if (std::isspace(c) || c < 0x20) return false;
    std::string message = "The SSH host key for " + identifier + " has changed" +
                          (print.empty() ? "" : " (new key " + print + ")") +
                          ". This can mean the server was reinstalled, or that someone is intercepting the "
                          "connection. Replace the stored key? ssh-keygen keeps a .old backup.";
    if (!hooks.confirm || !hooks.confirm("SSH host key changed", message)) return false;
    std::string keygen = find_keygen();
    for (const auto &file : files) {
        if (run({keygen, "-R", identifier, "-f", file}) != 0)
            throw Error("Could not remove the changed host key from " + file);
        if (hooks.log) hooks.log("Removed old host key for " + identifier + " from " + file + " (backup " + file + ".old)");
    }
    return true;
}

}  // namespace ptz
