// Login vault: SSH logins and camera logins in one file, two independent stores.
//
// The whole list is sealed with XChaCha20-Poly1305 (Monocypher) under a key derived
// from the Administrator Password with Argon2id, or kept as plain text by the
// operator's explicit choice. The Administrator Password itself is never stored.
//
//   PTZ Control logins 1
//   plain                                   or  argon2id <blocks> <passes> <salt> <nonce>
//   <payload lines>                         or  <hex of mac + ciphertext>
//
// Payload lines are "ssh" or "camera", then the login's name, username and password,
// separated by tabs, with \\, \t, \n and \r escaped. Lines without a name (written before
// names existed) take the username as their name.
#include "ptzcore.h"
#include "internal.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <stdlib.h>
#else
#include <sys/random.h>
#endif
#endif

#include "../third_party/monocypher/monocypher.h"

namespace {

constexpr const char *kMagic = "PTZ Control logins 1";
constexpr uint32_t kBlocks = 65536;  // 64 MiB of memory per derivation
constexpr uint32_t kPasses = 3;
constexpr size_t kKey = 32, kSalt = 16, kNonce = 24, kMac = 16;

struct Login {
    std::string name, user, password;
    ~Login() { crypto_wipe(password.data(), password.size()); }
    Login() = default;
    Login(const Login &) = default;
    Login &operator=(const Login &) = default;
};

void wipe(std::string &s) {
    crypto_wipe(s.data(), s.size());
    s.clear();
}

void os_random(uint8_t *out, size_t size) {
#if defined(_WIN32)
    if (BCryptGenRandom(nullptr, out, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw ptz::Error("Cannot obtain random bytes");
#elif defined(__APPLE__)
    arc4random_buf(out, size);
#else
    while (size) {
        ssize_t got = getrandom(out, size, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            throw ptz::Error("Cannot obtain random bytes");
        }
        out += got;
        size -= static_cast<size_t>(got);
    }
#endif
}

std::string hex(const uint8_t *data, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 15];
    }
    return out;
}

bool unhex(const std::string &text, std::vector<uint8_t> &out) {
    if (text.size() % 2) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.assign(text.size() / 2, 0);
    for (size_t i = 0; i < out.size(); ++i) {
        int hi = nibble(text[2 * i]), lo = nibble(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return true;
}

std::string escape(const std::string &text) {
    std::string out;
    for (char c : text) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\t': out += "\\t"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        default: out += c;
        }
    }
    return out;
}

std::string unescape(const std::string &text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 == text.size()) {
            out += text[i];
            continue;
        }
        char c = text[++i];
        out += c == 't' ? '\t' : c == 'n' ? '\n' : c == 'r' ? '\r' : c;
    }
    return out;
}

std::vector<std::string> split(const std::string &line, char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t at; (at = line.find(separator, start)) != std::string::npos; start = at + 1)
        parts.push_back(line.substr(start, at - start));
    parts.push_back(line.substr(start));
    return parts;
}

bool same_key(const Login &a, const Login &b) { return ptz::lower(a.name) == ptz::lower(b.name); }

void copy_error(char *out, size_t size, const std::string &text) {
    if (!out || !size) return;
    std::strncpy(out, text.c_str(), size - 1);
    out[size - 1] = '\0';
}

}  // namespace

struct ptz_vault {
    std::string path;
    ptz_vault_state state = PTZ_VAULT_NEW;
    std::vector<Login> stores[2];
    // Sealed vaults: the derivation parameters, and the key while unlocked.
    uint32_t blocks = kBlocks, passes = kPasses;
    uint8_t salt[kSalt] = {};
    uint8_t key[kKey] = {};
    std::vector<uint8_t> sealed;  // mac + ciphertext, kept while locked
    std::string header;           // authenticated with the ciphertext

    ~ptz_vault() { forget(); }

    void forget() {
        crypto_wipe(key, sizeof key);
        for (auto &store : stores) store.clear();
    }

    std::string payload() const {
        std::string out;
        for (int s = 0; s < 2; ++s)
            for (const Login &l : stores[s])
                out += std::string(s == PTZ_STORE_SSH ? "ssh" : "camera") + '\t' + escape(l.name) + '\t' +
                       escape(l.user) + '\t' + escape(l.password) + '\n';
        return out;
    }

    void parse_payload(const std::string &text) {
        std::vector<Login> parsed[2];
        std::istringstream in(text);
        for (std::string line; std::getline(in, line);) {
            if (line.empty()) continue;
            auto parts = split(line, '\t');
            if (parts.size() == 3) parts.insert(parts.begin() + 1, parts[1]);  // no name yet: the username
            if (parts.size() != 4 || (parts[0] != "ssh" && parts[0] != "camera") || parts[1].empty())
                throw ptz::Error("The saved logins file is damaged");
            Login l;
            l.name = unescape(parts[1]);
            l.user = unescape(parts[2]);
            l.password = unescape(parts[3]);
            parsed[parts[0] == "ssh" ? 0 : 1].push_back(l);
            wipe(line);
        }
        stores[0] = std::move(parsed[0]);
        stores[1] = std::move(parsed[1]);
    }

    void derive(const std::string &password) {
        std::vector<uint8_t> work(static_cast<size_t>(blocks) * 1024);
        crypto_argon2_config config = {CRYPTO_ARGON2_ID, blocks, passes, 1};
        crypto_argon2_inputs inputs = {reinterpret_cast<const uint8_t *>(password.data()), salt,
                                       static_cast<uint32_t>(password.size()), kSalt};
        crypto_argon2(key, kKey, work.data(), config, inputs, crypto_argon2_no_extras);
        crypto_wipe(work.data(), work.size());
    }

    void load() {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            state = PTZ_VAULT_NEW;
            return;
        }
        std::string magic, mode, body;
        std::getline(in, magic);
        std::getline(in, mode);
        std::stringstream rest;
        rest << in.rdbuf();
        body = rest.str();
        if (magic != kMagic) throw ptz::Error("The saved logins file isn't one this version can read");
        if (mode == "plain") {
            parse_payload(body);
            wipe(body);
            state = PTZ_VAULT_PLAIN;
            return;
        }
        auto parts = split(mode, ' ');
        const ptz::Error damaged("The saved logins file is damaged");
        if (parts.size() != 5 || parts[0] != "argon2id") throw damaged;
        auto whole = [&](const std::string &text, double low, double high) -> uint32_t {
            double value = ptz::parse_number(text).value_or(-1);
            if (value < low || value > high || value != static_cast<uint32_t>(value)) throw damaged;
            return static_cast<uint32_t>(value);
        };
        blocks = whole(parts[1], 8, 1 << 18);  // at most 256 MiB
        passes = whole(parts[2], 1, 16);
        std::vector<uint8_t> salt_bytes, nonce_bytes;
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
        if (!unhex(parts[3], salt_bytes) || salt_bytes.size() != kSalt || !unhex(parts[4], nonce_bytes) ||
            nonce_bytes.size() != kNonce || !unhex(body, sealed) || sealed.size() < kMac)
            throw damaged;
        std::memcpy(salt, salt_bytes.data(), kSalt);
        header = magic + "\n" + mode + "\n";
        state = PTZ_VAULT_LOCKED;
    }

    void unlock(const std::string &password) {
        derive(password);
        std::vector<uint8_t> nonce;
        unhex(split(header.substr(0, header.size() - 1), ' ').back(), nonce);
        std::string plain(sealed.size() - kMac, '\0');
        if (crypto_aead_unlock(reinterpret_cast<uint8_t *>(plain.data()), sealed.data(), key, nonce.data(),
                               reinterpret_cast<const uint8_t *>(header.data()), header.size(),
                               sealed.data() + kMac, plain.size()) != 0) {
            crypto_wipe(key, sizeof key);
            throw ptz::Error("The Administrator Password is wrong");
        }
        try {
            parse_payload(plain);
        } catch (...) {
            wipe(plain);
            crypto_wipe(key, sizeof key);
            throw;
        }
        wipe(plain);
        state = PTZ_VAULT_UNLOCKED;
    }

    void write() {
        std::string text = std::string(kMagic) + "\n";
        std::string body = payload();
        if (state == PTZ_VAULT_PLAIN) {
            text += "plain\n" + body;
        } else {
            uint8_t nonce[kNonce];
            os_random(nonce, sizeof nonce);
            std::string mode = "argon2id " + std::to_string(blocks) + " " + std::to_string(passes) + " " +
                               hex(salt, kSalt) + " " + hex(nonce, kNonce);
            header = text + mode + "\n";
            std::vector<uint8_t> out(kMac + body.size());
            crypto_aead_lock(out.data() + kMac, out.data(), key, nonce,
                             reinterpret_cast<const uint8_t *>(header.data()), header.size(),
                             reinterpret_cast<const uint8_t *>(body.data()), body.size());
            sealed = out;
            text = header + hex(out.data(), out.size()) + "\n";
        }
        wipe(body);
        std::string temp = path + ".new";
#if defined(_WIN32)
        std::wstring wtemp = ptz::widen(temp), wpath = ptz::widen(path);
        HANDLE file = CreateFileW(wtemp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            wipe(text);
            throw ptz::Error("Cannot save the logins: " + ptz::windows_error(GetLastError()));
        }
        DWORD written = 0;
        BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                  written == text.size() && FlushFileBuffers(file);
        CloseHandle(file);
        wipe(text);
        if (!ok || !MoveFileExW(wtemp.c_str(), wpath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DWORD code = GetLastError();
            DeleteFileW(wtemp.c_str());
            throw ptz::Error("Cannot save the logins: " + ptz::windows_error(code));
        }
#else
        int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) {
            wipe(text);
            throw ptz::Error(std::string("Cannot save the logins: ") + std::strerror(errno));
        }
        fchmod(fd, 0600);
        size_t done = 0;
        while (done < text.size()) {
            ssize_t n = ::write(fd, text.data() + done, text.size() - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            done += static_cast<size_t>(n);
        }
        bool ok = done == text.size() && fsync(fd) == 0;
        ::close(fd);
        wipe(text);
        if (!ok || ::rename(temp.c_str(), path.c_str()) != 0) {
            int code = errno;
            ::unlink(temp.c_str());
            throw ptz::Error(std::string("Cannot save the logins: ") + std::strerror(code));
        }
#endif
    }

    bool open_for_changes() const { return state == PTZ_VAULT_PLAIN || state == PTZ_VAULT_UNLOCKED; }
};

extern "C" {

ptz_vault *ptz_vault_open(const char *path, char *error, size_t error_size) {
    if (!path || !*path) {
        copy_error(error, error_size, "No place to keep saved logins");
        return nullptr;
    }
    auto vault = std::make_unique<ptz_vault>();
    vault->path = path;
    try {
        vault->load();
    } catch (const std::exception &e) {
        copy_error(error, error_size, e.what());
        return nullptr;
    }
    return vault.release();
}

void ptz_vault_free(ptz_vault *vault) { delete vault; }

ptz_vault_state ptz_vault_get_state(const ptz_vault *vault) { return vault ? vault->state : PTZ_VAULT_NEW; }

int ptz_vault_unlock(ptz_vault *vault, const char *admin_password, char *error, size_t error_size) {
    if (!vault) return 0;
    if (vault->state != PTZ_VAULT_LOCKED) return vault->open_for_changes();
    try {
        vault->unlock(admin_password ? admin_password : "");
        return 1;
    } catch (const std::exception &e) {
        copy_error(error, error_size, e.what());
        return 0;
    }
}

void ptz_vault_lock(ptz_vault *vault) {
    if (!vault || vault->state != PTZ_VAULT_UNLOCKED) return;
    vault->forget();
    vault->state = PTZ_VAULT_LOCKED;
}

int ptz_vault_set_admin_password(ptz_vault *vault, const char *admin_password, char *error, size_t error_size) {
    if (!vault) return 0;
    if (vault->state == PTZ_VAULT_LOCKED) {
        copy_error(error, error_size, "Unlock the saved logins first");
        return 0;
    }
    std::string password = admin_password ? admin_password : "";
    ptz_vault_state previous = vault->state;
    uint8_t previous_key[kKey], previous_salt[kSalt];
    std::memcpy(previous_key, vault->key, kKey);
    std::memcpy(previous_salt, vault->salt, kSalt);
    try {
        if (password.empty()) {
            vault->state = PTZ_VAULT_PLAIN;
            crypto_wipe(vault->key, kKey);
        } else {
            os_random(vault->salt, kSalt);
            vault->blocks = kBlocks;
            vault->passes = kPasses;
            vault->derive(password);
            vault->state = PTZ_VAULT_UNLOCKED;
        }
        wipe(password);
        vault->write();
    } catch (const std::exception &e) {
        wipe(password);
        vault->state = previous;
        std::memcpy(vault->key, previous_key, kKey);
        std::memcpy(vault->salt, previous_salt, kSalt);
        crypto_wipe(previous_key, kKey);
        copy_error(error, error_size, e.what());
        return 0;
    }
    crypto_wipe(previous_key, kKey);
    return 1;
}

size_t ptz_vault_count(const ptz_vault *vault, ptz_store store) {
    if (!vault || !vault->open_for_changes() || (store != PTZ_STORE_SSH && store != PTZ_STORE_CAMERA)) return 0;
    return vault->stores[store].size();
}

int ptz_vault_login(const ptz_vault *vault, ptz_store store, size_t index, ptz_login *out) {
    if (!out || index >= ptz_vault_count(vault, store)) return 0;
    const Login &l = vault->stores[store][index];
    *out = {l.name.c_str(), l.user.c_str(), l.password.c_str()};
    return 1;
}

long ptz_vault_save(ptz_vault *vault, ptz_store store, long index, const ptz_login *login, char *error,
                    size_t error_size) {
    if (!vault || !login || (store != PTZ_STORE_SSH && store != PTZ_STORE_CAMERA)) return -1;
    if (!vault->open_for_changes()) {
        copy_error(error, error_size, vault->state == PTZ_VAULT_LOCKED ? "Unlock the saved logins first"
                                                                       : "Choose how to keep saved logins first");
        return -1;
    }
    Login l;
    l.name = ptz::trim(login->name ? login->name : "");
    l.user = ptz::trim(login->user ? login->user : "");
    l.password = login->password ? login->password : "";
    if (l.name.empty() || l.user.empty()) {
        copy_error(error, error_size, "A login needs a name and a username");
        return -1;
    }
    auto &list = vault->stores[store];
    if (index >= static_cast<long>(list.size())) index = -1;
    std::vector<Login> before = list;
    if (index >= 0) list.erase(list.begin() + index);
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Login &o) { return same_key(o, l); }), list.end());
    list.push_back(l);
    std::stable_sort(list.begin(), list.end(), [](const Login &a, const Login &b) {
        std::string la = ptz::lower(a.name), lb = ptz::lower(b.name);
        return la != lb ? la < lb : a.name < b.name;
    });
    try {
        vault->write();
    } catch (const std::exception &e) {
        list = before;
        copy_error(error, error_size, e.what());
        return -1;
    }
    for (size_t i = 0; i < list.size(); ++i)
        if (same_key(list[i], l)) return static_cast<long>(i);
    return -1;
}

int ptz_vault_remove(ptz_vault *vault, ptz_store store, size_t index, char *error, size_t error_size) {
    if (index >= ptz_vault_count(vault, store)) return 0;
    auto &list = vault->stores[store];
    std::vector<Login> before = list;
    list.erase(list.begin() + static_cast<long>(index));
    try {
        vault->write();
    } catch (const std::exception &e) {
        list = before;
        copy_error(error, error_size, e.what());
        return 0;
    }
    return 1;
}

}  // extern "C"
