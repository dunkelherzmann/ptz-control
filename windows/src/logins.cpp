// Saved SSH and camera logins (spec §17): the core's login vault, its dialogs and the
// Manage Logins window. Mirrors macos/Sources/PTZControl/LoginVault.swift and ManageLogins.swift.
#include "app.h"

#include <commctrl.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace {

struct Vault {
    ptz_vault *handle = nullptr;
    std::wstring open_error;
    std::vector<std::function<void()>> listeners;

    Vault() {
        std::wstring path;
        if (const wchar_t *override_path = _wgetenv(L"PTZ_VAULT_PATH"); override_path && *override_path) {
            path = override_path;
        } else {
            // %APPDATA%\PTZControl\logins.vault: the user's own profile, readable only by them.
            wchar_t appdata[MAX_PATH];
            DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
            std::wstring folder = (n && n < MAX_PATH ? std::wstring(appdata) : std::wstring(L".")) + L"\\PTZControl";
            CreateDirectoryW(folder.c_str(), nullptr);
            path = folder + L"\\logins.vault";
        }
        char error[PTZ_TEXT] = "";
        handle = ptz_vault_open(U(path).c_str(), error, sizeof error);
        if (!handle) open_error = W(error);
    }

    void changed() {
        for (auto &fn : listeners) fn();
    }
};

Vault &vault() {
    static Vault v;
    return v;
}

ptz_store to_store(LoginStore s) { return s == LoginStore::Ssh ? PTZ_STORE_SSH : PTZ_STORE_CAMERA; }

bool call(std::wstring *error, const std::function<bool(char *, size_t)> &fn) {
    if (!vault().handle) {
        if (error) *error = vault().open_error.empty() ? L"Saved logins are unavailable" : vault().open_error;
        return false;
    }
    char buffer[PTZ_TEXT] = "";
    bool ok = fn(buffer, sizeof buffer);
    if (!ok && error) *error = W(buffer);
    return ok;
}

void zero(std::wstring &s) {
    SecureZeroMemory(s.data(), s.size() * sizeof(wchar_t));
    s.clear();
}

void zero(std::string &s) {
    SecureZeroMemory(s.data(), s.size());
    s.clear();
}

std::wstring window_text(HWND w) {
    int n = GetWindowTextLengthW(w);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(w, s.data(), n + 1);
    s.resize(static_cast<size_t>(n));
    return s;
}

std::wstring trim(const std::wstring &s) {
    size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
    return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

int scale(HWND w, int v) { return MulDiv(v, static_cast<int>(w ? GetDpiForWindow(w) : GetDpiForSystem()), 96); }

HFONT message_font(HWND w) {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof metrics;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0,
                               w ? GetDpiForWindow(w) : GetDpiForSystem());
    return CreateFontIndirectW(&metrics.lfMessageFont);
}

void init_controls() {
    static bool done = [] {
        INITCOMMONCONTROLSEX icc{sizeof(INITCOMMONCONTROLSEX), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
        return InitCommonControlsEx(&icc) != FALSE;
    }();
    (void)done;
}

HWND make_tooltips(HWND owner) {
    init_controls();
    HWND tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0,
                                0, 0, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(tips, TTM_SETMAXTIPWIDTH, 0, scale(owner, 360));
    return tips;
}

// Runs a window as a modal dialog over `owner` until `*done`, then destroys it.
void run_modal(HWND owner, HWND dialog, const bool *done, HWND focus) {
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    SetForegroundWindow(dialog);
    if (focus) SetFocus(focus);
    MSG msg;
    while (!*done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(dialog, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (owner) EnableWindow(owner, TRUE);
    DestroyWindow(dialog);
    if (owner) SetForegroundWindow(owner);
}

HWND center_window(HWND owner, const wchar_t *cls, const std::wstring &title, int client_w, int client_h, DWORD style,
                   DWORD ex_style) {
    RECT frame = {0, 0, client_w, client_h};
    AdjustWindowRectExForDpi(&frame, style, FALSE, ex_style, owner ? GetDpiForWindow(owner) : GetDpiForSystem());
    int w = frame.right - frame.left, h = frame.bottom - frame.top;
    RECT o{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (owner) GetWindowRect(owner, &o);
    return CreateWindowExW(ex_style, cls, title.c_str(), style, o.left + (o.right - o.left - w) / 2,
                           o.top + (o.bottom - o.top - h) / 3, w, h, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
}

// ---- a small modal form: a message, text fields, an optional Show password box, buttons ----------

struct Field {
    std::wstring label, value, cue;
    bool secret = false;
};

struct Form {
    std::wstring title, message;
    std::vector<Field> fields;
    bool show_password = false;
    std::vector<std::wstring> buttons;  // left to right
    int default_button = 0, cancel_button = -1;
};

struct FormRun {
    const Form *form;
    std::vector<HWND> edits;
    std::vector<std::wstring> values;
    HWND show = nullptr;
    int pressed = -1;
    bool done = false;
};

constexpr int kButton = 1000, kShow = 900;

LRESULT CALLBACK form_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto *run = reinterpret_cast<FormRun *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (!run) break;
            if (id == kShow) {
                bool on = SendMessageW(run->show, BM_GETCHECK, 0, 0) == BST_CHECKED;
                for (size_t i = 0; i < run->edits.size(); ++i)
                    if (run->form->fields[i].secret) {
                        SendMessageW(run->edits[i], EM_SETPASSWORDCHAR, on ? 0 : 0x25CF, 0);
                        InvalidateRect(run->edits[i], nullptr, TRUE);
                    }
                return 0;
            }
            if (id == IDOK || id == IDCANCEL || id >= kButton) {
                run->pressed = id == IDOK       ? run->form->default_button
                               : id == IDCANCEL ? run->form->cancel_button
                                                : id - kButton;
                for (HWND e : run->edits) {
                    run->values.push_back(window_text(e));
                    SetWindowTextW(e, L"");
                }
                run->done = true;
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            if (run) {
                run->pressed = run->form->cancel_button;
                for (HWND e : run->edits) SetWindowTextW(e, L"");
                run->done = true;
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Returns the button pressed (the cancel button when closed) and the fields' text.
int run_form(HWND owner, const Form &form, std::vector<std::wstring> &values) {
    init_controls();
    static bool registered = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = form_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = L"PTZForm";
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        return RegisterClassW(&wc) != 0;
    }();
    (void)registered;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    auto S = [&](int v) { return scale(owner, v); };
    bool labels = std::any_of(form.fields.begin(), form.fields.end(), [](const Field &f) { return !f.label.empty(); });
    int width = S(460), label_w = labels ? S(90) : 0;
    int msg_h = form.message.empty() ? 0 : (form.message.size() > 160 ? S(112) : S(58));
    int fields_h = static_cast<int>(form.fields.size()) * S(34) + (form.show_password ? S(28) : 0);
    int client_h = S(16) + msg_h + fields_h + S(56);
    FormRun run{&form};
    HWND dialog = center_window(owner, L"PTZForm", form.title, width, client_h, WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT);
    SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&run));
    HFONT font = message_font(owner);
    std::vector<HWND> controls;
    int y = S(16);
    if (!form.message.empty()) {
        controls.push_back(CreateWindowExW(0, L"STATIC", form.message.c_str(), WS_CHILD | WS_VISIBLE, S(16), y,
                                           width - S(32), msg_h - S(6), dialog, nullptr, instance, nullptr));
        y += msg_h;
    }
    for (const Field &f : form.fields) {
        if (labels)
            controls.push_back(CreateWindowExW(0, L"STATIC", f.label.c_str(), WS_CHILD | WS_VISIBLE | SS_RIGHT, S(16),
                                               y + S(4), label_w - S(8), S(20), dialog, nullptr, instance, nullptr));
        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | (f.secret ? ES_PASSWORD : 0),
                                    S(16) + label_w, y, width - S(32) - label_w, S(26), dialog, nullptr, instance, nullptr);
        if (!f.cue.empty()) SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(f.cue.c_str()));
        SetWindowTextW(edit, f.value.c_str());
        run.edits.push_back(edit);
        controls.push_back(edit);
        y += S(34);
    }
    HWND tips = nullptr;
    if (form.show_password) {
        run.show = CreateWindowExW(0, L"BUTTON", L"Show password", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                   S(16) + label_w, y, S(200), S(22), dialog,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kShow)), instance, nullptr);
        controls.push_back(run.show);
        tips = make_tooltips(dialog);
        add_tooltip(tips, run.show, L"Show password — Shows the saved password as text while this form is open.");
    }
    int bx = width - S(16);
    for (int i = static_cast<int>(form.buttons.size()) - 1; i >= 0; --i) {
        HDC dc = GetDC(dialog);
        HGDIOBJ old = SelectObject(dc, font);
        SIZE size{};
        GetTextExtentPoint32W(dc, form.buttons[i].c_str(), static_cast<int>(form.buttons[i].size()), &size);
        SelectObject(dc, old);
        ReleaseDC(dialog, dc);
        int bw = std::max<int>(S(84), size.cx + S(24));
        bx -= bw;
        HWND b = CreateWindowExW(0, L"BUTTON", form.buttons[i].c_str(),
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | (i == form.default_button ? BS_DEFPUSHBUTTON : 0),
                                 bx, client_h - S(44), bw, S(28), dialog,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButton + i)), instance, nullptr);
        controls.push_back(b);
        bx -= S(8);
    }
    for (HWND c : controls) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    HWND focus = run.edits.empty() ? GetDlgItem(dialog, kButton + form.default_button) : run.edits.front();
    run_modal(owner, dialog, &run.done, focus);
    if (tips) DestroyWindow(tips);
    DeleteObject(font);
    values = std::move(run.values);
    values.resize(form.fields.size());
    return run.pressed;
}

int ask(HWND owner, const std::wstring &title, const std::wstring &message, std::vector<std::wstring> buttons,
        int default_button, int cancel_button) {
    Form form{title, message, {}, false, std::move(buttons), default_button, cancel_button};
    std::vector<std::wstring> values;
    return run_form(owner, form, values);
}

void show_error(HWND owner, const std::wstring &title, const std::wstring &message) {
    ask(owner, title, message, {L"OK"}, 0, 0);
}

bool confirm_plain_text(HWND owner) {
    return ask(owner, L"Store passwords as plain text?",
               L"Saved SSH and camera passwords will be readable by anyone who can read your files.",
               {L"Store as Plain Text", L"Cancel"}, 1, 1) == 0;
}

bool choose_storage(HWND owner) {
    int choice = ask(owner, L"Protect Saved Logins",
                     L"Saved logins hold passwords. An Administrator Password encrypts your saved SSH and camera "
                     L"logins; you enter it once each time PTZ Control starts. It can't be recovered if you forget it.\n\n"
                     L"Without one, passwords are stored as plain text, which anyone who can read your files can see.",
                     {L"Store as Plain Text…", L"Not Now", L"Create Administrator Password…"}, 2, 1);
    if (choice == 2) return vault_change_admin_dialog(owner);
    if (choice != 0 || !confirm_plain_text(owner)) return false;
    std::wstring error;
    if (!call(&error, [](char *buffer, size_t size) {
            return ptz_vault_set_admin_password(vault().handle, nullptr, buffer, size) != 0;
        })) {
        show_error(owner, L"Can't save logins", error);
        return false;
    }
    vault().changed();
    return true;
}

// The New and Edit form: username and password, with Show password.
std::optional<SavedLogin> login_form(HWND owner, const SavedLogin &login, LoginStore store, const std::wstring &title) {
    bool ssh = store == LoginStore::Ssh;
    std::wstring note = ssh ? L"The password may stay blank for keys or ssh-agent." : L"";
    std::wstring name = login.name, user = login.user, password = login.password;
    while (true) {
        Form form{title, note,
                  {{L"Name", name, ssh ? L"e.g. jump host" : L"e.g. cam1", false},
                   {L"Username", user, ssh ? L"username" : L"admin", false},
                   {L"Password", password, ssh ? L"blank for keys / agent" : L"camera password", true}},
                  true, {L"Save", L"Cancel"}, 0, 1};
        zero(password);
        std::vector<std::wstring> values;
        int pressed = run_form(owner, form, values);
        for (Field &f : form.fields) zero(f.value);
        if (pressed != 0) {
            for (auto &v : values) zero(v);
            return std::nullopt;
        }
        name = trim(values[0]);
        user = trim(values[1]);
        password = values[2];
        zero(values[2]);
        if (name.empty() || user.empty()) {
            note = L"A name and a username are required.";
            continue;
        }
        SavedLogin result{name, user, password};
        zero(password);
        return result;
    }
}

// ---- Manage Logins ----------------------------------------------------------------------------

enum : int {
    ML_SSH = 300, ML_CAMERA, ML_NEW, ML_EDIT, ML_DELETE, ML_LOCK, ML_ADMIN, ML_DONE, ML_LIST, ML_STATUS,
};

struct Manage {
    HWND hwnd = nullptr, ssh = nullptr, camera = nullptr, list = nullptr, status = nullptr, tips = nullptr;
    HWND new_ = nullptr, edit = nullptr, del = nullptr, lock = nullptr, admin = nullptr, done_button = nullptr;
    HFONT font = nullptr, mono = nullptr;
    LoginStore store = LoginStore::Ssh;
    std::vector<SavedLogin> logins;
    bool done = false;
};

Manage *g_manage = nullptr;
LoginStore g_last_store = LoginStore::Ssh;  // the list Manage Logins showed last

int selected_row(Manage &m) { return static_cast<int>(SendMessageW(m.list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED)); }

void manage_reload(Manage &m) {
    int keep = selected_row(m);
    for (auto &l : m.logins) zero(l.password);
    m.logins = vault_logins(m.store);
    SendMessageW(m.list, LVM_DELETEALLITEMS, 0, 0);
    for (int i = 0; i < static_cast<int>(m.logins.size()); ++i) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = const_cast<wchar_t *>(m.logins[i].name.c_str());
        SendMessageW(m.list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
        item.iSubItem = 1;
        item.pszText = const_cast<wchar_t *>(m.logins[i].user.c_str());
        SendMessageW(m.list, LVM_SETITEMTEXTW, i, reinterpret_cast<LPARAM>(&item));
        std::wstring hidden = m.logins[i].password.empty() ? L"—" : L"••••••";
        item.iSubItem = 2;
        item.pszText = hidden.data();
        SendMessageW(m.list, LVM_SETITEMTEXTW, i, reinterpret_cast<LPARAM>(&item));
    }
    if (keep >= 0 && keep < static_cast<int>(m.logins.size())) {
        LVITEMW state{};
        state.stateMask = state.state = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(m.list, LVM_SETITEMSTATE, keep, reinterpret_cast<LPARAM>(&state));
    }
    SendMessageW(m.ssh, BM_SETCHECK, m.store == LoginStore::Ssh ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(m.camera, BM_SETCHECK, m.store == LoginStore::Camera ? BST_CHECKED : BST_UNCHECKED, 0);
    VaultState state = vault_state();
    SetWindowTextW(m.admin, state == VaultState::Locked || state == VaultState::Unlocked
                                ? L"Change Administrator Password…"
                                : L"Create Administrator Password…");
    SetWindowTextW(m.lock, state == VaultState::Locked ? L"Unlock…" : L"Lock");
    EnableWindow(m.lock, state == VaultState::Locked || state == VaultState::Unlocked);
    SetWindowTextW(m.status, state == VaultState::Locked     ? L"Locked."
                             : state == VaultState::Unlocked ? L"Encrypted with the Administrator Password."
                             : state == VaultState::Plain    ? L"Stored as plain text."
                             : state == VaultState::New      ? L"No logins saved yet."
                                                             : vault_open_error().c_str());
    bool usable = state == VaultState::Plain || state == VaultState::Unlocked || state == VaultState::New;
    bool chosen = usable && selected_row(m) >= 0;
    EnableWindow(m.new_, usable);
    EnableWindow(m.edit, chosen);
    EnableWindow(m.del, chosen);
}

void manage_save(Manage &m, const SavedLogin &login, int row) {
    std::wstring error;
    int index = -1;
    if (!vault_save(login, m.store, row, &error, &index)) {
        show_error(m.hwnd, L"Can't save the login", error);
        return;
    }
    manage_reload(m);
    LVITEMW state{};
    state.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(m.list, LVM_SETITEMSTATE, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(&state));
    state.state = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(m.list, LVM_SETITEMSTATE, index, reinterpret_cast<LPARAM>(&state));
    manage_reload(m);
}

void manage_command(Manage &m, int id) {
    bool ssh = m.store == LoginStore::Ssh;
    switch (id) {
        case ML_SSH: case ML_CAMERA:
            m.store = id == ML_SSH ? LoginStore::Ssh : LoginStore::Camera;
            SendMessageW(m.list, LVM_DELETEALLITEMS, 0, 0);
            manage_reload(m);
            return;
        case ML_NEW: {
            if (!vault_ensure_writable(m.hwnd)) return;
            if (auto login = login_form(m.hwnd, {}, m.store, ssh ? L"New SSH Login" : L"New Camera Login")) {
                manage_save(m, *login, -1);
                zero(login->password);
            }
            return;
        }
        case ML_EDIT: {
            int row = selected_row(m);
            if (row < 0 || row >= static_cast<int>(m.logins.size()) || !vault_ensure_writable(m.hwnd)) return;
            if (auto login = login_form(m.hwnd, m.logins[row], m.store, ssh ? L"Edit SSH Login" : L"Edit Camera Login")) {
                manage_save(m, *login, row);
                zero(login->password);
            }
            return;
        }
        case ML_DELETE: {
            int row = selected_row(m);
            VaultState state = vault_state();
            if (row < 0 || row >= static_cast<int>(m.logins.size()) ||
                (state != VaultState::Plain && state != VaultState::Unlocked))
                return;
            if (ask(m.hwnd, L"Delete login",
                    L"Delete the login " + m.logins[row].name + L"? This removes it from the saved " +
                        (ssh ? L"SSH" : L"camera") + L" logins.",
                    {L"Delete", L"Cancel"}, 1, 1) != 0)
                return;
            std::wstring error;
            if (!vault_remove(row, m.store, &error)) show_error(m.hwnd, L"Can't delete the login", error);
            manage_reload(m);
            return;
        }
        case ML_LOCK:
            if (vault_state() == VaultState::Locked) vault_unlock_dialog(m.hwnd);
            else vault_lock();
            manage_reload(m);
            return;
        case ML_ADMIN:
            vault_change_admin_dialog(m.hwnd);
            manage_reload(m);
            return;
        case ML_DONE: case IDCANCEL:
            m.done = true;
            return;
    }
}

LRESULT CALLBACK manage_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Manage *m = g_manage && g_manage->hwnd == hwnd ? g_manage : nullptr;
    switch (msg) {
        case WM_COMMAND:
            if (m) manage_command(*m, LOWORD(wp));
            return 0;
        case WM_NOTIFY: {
            auto *hdr = reinterpret_cast<NMHDR *>(lp);
            if (!m || hdr->idFrom != ML_LIST) break;
            if (hdr->code == NM_DBLCLK) manage_command(*m, ML_EDIT);
            if (hdr->code == LVN_ITEMCHANGED) {
                bool chosen = selected_row(*m) >= 0 &&
                              (vault_state() == VaultState::Plain || vault_state() == VaultState::Unlocked);
                EnableWindow(m->edit, chosen);
                EnableWindow(m->del, chosen);
            }
            if (hdr->code == LVN_KEYDOWN) {
                WORD key = reinterpret_cast<NMLVKEYDOWN *>(lp)->wVKey;
                if (key == VK_DELETE || key == VK_BACK) manage_command(*m, ML_DELETE);
            }
            return 0;
        }
        case WM_CLOSE:
            if (m) m->done = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Builds the Manage Logins window (hidden) with every control in place.
Manage *manage_create(HWND owner, LoginStore store) {
    init_controls();
    static bool registered = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = manage_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        wc.lpszClassName = L"PTZManageLogins";
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
        return RegisterClassW(&wc) != 0;
    }();
    (void)registered;
    auto *m = new Manage;
    m->store = store;
    g_manage = m;
    auto S = [&](int v) { return scale(owner, v); };
    int width = S(640), height = S(400);
    m->hwnd = center_window(owner, L"PTZManageLogins", L"Manage Logins", width, height,
                            WS_POPUP | WS_CAPTION | WS_SYSMENU, WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    auto button = [&](const wchar_t *text, int id, int x, int y, int w, DWORD extra = 0) {
        return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | extra, x, y, w, S(28), m->hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    };
    int pad = S(14);
    m->ssh = button(L"SSH Logins", ML_SSH, pad, pad, S(110), BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP);
    m->camera = button(L"Camera Logins", ML_CAMERA, pad + S(110), pad, S(120), BS_AUTORADIOBUTTON | BS_PUSHLIKE);
    m->del = button(L"Delete", ML_DELETE, width - pad - S(84), pad, S(84), WS_GROUP);
    m->edit = button(L"Edit…", ML_EDIT, width - pad - S(176), pad, S(84));
    m->new_ = button(L"New…", ML_NEW, width - pad - S(268), pad, S(84));
    m->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"Saved logins",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, pad,
                              pad + S(40), width - 2 * pad, height - 2 * pad - S(84), m->hwnd,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(ML_LIST)), instance, nullptr);
    SendMessageW(m->list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.cx = S(210);
    column.pszText = const_cast<wchar_t *>(L"Name");
    SendMessageW(m->list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
    column.pszText = const_cast<wchar_t *>(L"Username");
    SendMessageW(m->list, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));
    column.cx = width - 2 * pad - S(430);
    column.pszText = const_cast<wchar_t *>(L"Password");
    SendMessageW(m->list, LVM_INSERTCOLUMNW, 2, reinterpret_cast<LPARAM>(&column));
    int by = height - pad - S(28);
    m->lock = button(L"Lock", ML_LOCK, pad, by, S(84), WS_GROUP);
    m->admin = button(L"Create Administrator Password…", ML_ADMIN, pad + S(92), by, S(230));
    m->status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, pad + S(332),
                                by + S(6), width - pad - S(92) - (pad + S(332)), S(20), m->hwnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ML_STATUS)), instance, nullptr);
    m->done_button = button(L"Done", ML_DONE, width - pad - S(84), by, S(84));
    m->font = message_font(owner);
    for (HWND c : {m->ssh, m->camera, m->new_, m->edit, m->del, m->list, m->lock, m->admin, m->status, m->done_button})
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(m->font), TRUE);
    m->tips = make_tooltips(m->hwnd);
    add_tooltip(m->tips, m->ssh, L"Saved SSH usernames and passwords");
    add_tooltip(m->tips, m->camera,
                L"Saved camera usernames and passwords. The two lists are kept apart, so any SSH login can be used "
                L"with any camera login.");
    add_tooltip(m->tips, m->new_, L"New login — Add a saved login to this list.");
    add_tooltip(m->tips, m->edit, L"Edit login — Change the selected login (or double-click it).");
    add_tooltip(m->tips, m->del, L"Delete login — Remove the selected login after a confirmation (Delete).");
    add_tooltip(m->tips, m->admin,
                L"Administrator Password — Encrypts every saved SSH and camera login. Change it, or leave the new "
                L"password blank to keep logins as plain text.");
    add_tooltip(m->tips, m->lock, L"Lock or unlock the saved logins with the Administrator Password.");
    add_tooltip(m->tips, m->done_button, L"Close Manage Logins (Esc).");
    manage_reload(*m);
    return m;
}

void manage_destroy(Manage *m) {
    if (m->tips) DestroyWindow(m->tips);
    if (m->hwnd && IsWindow(m->hwnd)) DestroyWindow(m->hwnd);
    DeleteObject(m->font);
    for (auto &l : m->logins) zero(l.password);
    if (g_manage == m) g_manage = nullptr;
    delete m;
}

}  // namespace

HWND add_tooltip(HWND tips, HWND control, const wchar_t *text) {
    TOOLINFOW info{};
    info.cbSize = sizeof info;
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = GetParent(control);
    info.uId = reinterpret_cast<UINT_PTR>(control);
    info.lpszText = const_cast<wchar_t *>(text);
    SendMessageW(tips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    return control;
}

HWND create_tooltips(HWND owner) { return make_tooltips(owner); }

VaultState vault_state() {
    if (!vault().handle) return VaultState::Unavailable;
    switch (ptz_vault_get_state(vault().handle)) {
        case PTZ_VAULT_PLAIN: return VaultState::Plain;
        case PTZ_VAULT_LOCKED: return VaultState::Locked;
        case PTZ_VAULT_UNLOCKED: return VaultState::Unlocked;
        default: return VaultState::New;
    }
}

std::wstring vault_open_error() { return vault().open_error; }

std::vector<SavedLogin> vault_logins(LoginStore store) {
    std::vector<SavedLogin> out;
    if (!vault().handle) return out;
    size_t count = ptz_vault_count(vault().handle, to_store(store));
    for (size_t i = 0; i < count; ++i) {
        ptz_login l{};
        if (ptz_vault_login(vault().handle, to_store(store), i, &l)) out.push_back({W(l.name), W(l.user), W(l.password)});
    }
    return out;
}

bool vault_save(const SavedLogin &login, LoginStore store, int replacing, std::wstring *error, int *index) {
    std::string name = U(login.name), user = U(login.user), password = U(login.password);
    long result = -1;
    bool ok = call(error, [&](char *buffer, size_t size) {
        ptz_login l{name.c_str(), user.c_str(), password.c_str()};
        result = ptz_vault_save(vault().handle, to_store(store), replacing, &l, buffer, size);
        return result >= 0;
    });
    zero(password);
    if (ok) {
        if (index) *index = static_cast<int>(result);
        vault().changed();
    }
    return ok;
}

bool vault_remove(int index, LoginStore store, std::wstring *error) {
    bool ok = call(error, [&](char *buffer, size_t size) {
        return ptz_vault_remove(vault().handle, to_store(store), static_cast<size_t>(index), buffer, size) != 0;
    });
    if (ok) vault().changed();
    return ok;
}

void vault_lock() {
    ptz_vault_lock(vault().handle);
    vault().changed();
}

void vault_subscribe(std::function<void()> fn) { vault().listeners.push_back(std::move(fn)); }

std::wstring vault_ask_name(HWND owner, LoginStore store, const std::wstring &user, const std::wstring &suggested) {
    bool ssh = store == LoginStore::Ssh;
    std::wstring note = std::wstring(L"Name the ") + (ssh ? L"SSH" : L"camera") + L" login for " + user +
                        L", to find it in the Login menu. A saved login with the same name is updated.";
    std::wstring name = suggested;
    while (true) {
        Form form{ssh ? L"Save SSH Login" : L"Save Camera Login", note,
                  {{L"", name, ssh ? L"e.g. jump host" : L"e.g. cam1", false}}, false, {L"Save", L"Don't Save"}, 0, 1};
        std::vector<std::wstring> values;
        if (run_form(owner, form, values) != 0) return L"";
        name = trim(values[0]);
        if (!name.empty()) return name;
        note = L"Enter a name, or Don't Save.";
    }
}

bool vault_unlock_dialog(HWND owner) {
    std::wstring note = L"Enter the Administrator Password to use your saved SSH and camera logins.";
    while (vault_state() == VaultState::Locked) {
        Form form{L"Unlock Saved Logins", note, {{L"", L"", L"Administrator Password", true}}, false,
                  {L"Unlock", L"Cancel"}, 0, 1};
        std::vector<std::wstring> values;
        if (run_form(owner, form, values) != 0) return false;
        std::string answer = U(values[0]);
        zero(values[0]);
        std::wstring error;
        if (call(&error, [&](char *buffer, size_t size) {
                return ptz_vault_unlock(vault().handle, answer.c_str(), buffer, size) != 0;
            }))
            vault().changed();
        else
            note = error + L". Try again.";
        zero(answer);
    }
    return true;
}

bool vault_change_admin_dialog(HWND owner) {
    if (vault_state() == VaultState::Locked && !vault_unlock_dialog(owner)) return false;
    if (vault_state() == VaultState::Unavailable) return false;
    bool changing = vault_state() == VaultState::Unlocked;
    std::wstring note = changing ? L"Every saved login is encrypted again with the new password. Leave both fields "
                                   L"blank to remove the Administrator Password."
                                 : L"It encrypts your saved SSH and camera logins. It can't be recovered if you forget it.";
    while (true) {
        Form form{changing ? L"Change Administrator Password" : L"Create Administrator Password", note,
                  {{L"", L"", L"New Administrator Password", true}, {L"", L"", L"Repeat it", true}}, false,
                  {changing ? L"Change" : L"Create", L"Cancel"}, 0, 1};
        std::vector<std::wstring> values;
        if (run_form(owner, form, values) != 0) {
            for (auto &v : values) zero(v);
            return false;
        }
        bool same = values[0] == values[1], empty = values[0].empty();
        std::string password = U(values[0]);
        for (auto &v : values) zero(v);
        if (!same) {
            note = L"The two passwords don't match. Try again.";
            continue;
        }
        if (empty) {
            if (!changing) {
                note = L"Enter a password, or Cancel.";
                continue;
            }
            if (!confirm_plain_text(owner)) return false;
        }
        std::wstring error;
        bool done = call(&error, [&](char *buffer, size_t size) {
            return ptz_vault_set_admin_password(vault().handle, password.c_str(), buffer, size) != 0;
        });
        zero(password);
        if (!done) {
            show_error(owner, L"Can't change the Administrator Password", error);
            return false;
        }
        vault().changed();
        return true;
    }
}

bool vault_ensure_readable(HWND owner) {
    switch (vault_state()) {
        case VaultState::Locked: return vault_unlock_dialog(owner);
        case VaultState::Unavailable: show_error(owner, L"Saved logins are unavailable", vault_open_error()); return false;
        default: return true;
    }
}

bool vault_ensure_writable(HWND owner) {
    if (vault_state() == VaultState::New) return choose_storage(owner);
    return vault_ensure_readable(owner);
}

void show_manage_logins(HWND owner, std::optional<LoginStore> store) {
    if (g_manage) return;
    if (vault_state() == VaultState::Unavailable) {
        show_error(owner, L"Saved logins are unavailable", vault_open_error());
        return;
    }
    if (vault_state() == VaultState::Locked) vault_unlock_dialog(owner);
    Manage *m = manage_create(owner, store.value_or(g_last_store));
    run_modal(owner, m->hwnd, &m->done, m->list);
    g_last_store = m->store;
    m->hwnd = nullptr;  // run_modal destroyed it
    manage_destroy(m);
}

HWND manage_logins_for_snapshot(HWND owner, LoginStore store) {
    Manage *m = manage_create(owner, store);
    return m->hwnd;
}

void manage_logins_close_snapshot() {
    if (g_manage) manage_destroy(g_manage);
}
