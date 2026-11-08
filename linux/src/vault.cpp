// Saved SSH and camera logins (spec §17): the core's login vault, its dialogs and
// the Manage Logins window. Mirrors macos/Sources/PTZControl/LoginVault.swift and
// ManageLogins.swift.
#include "app.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QVBoxLayout>

#include <vector>

namespace {

struct Vault {
    ptz_vault *handle = nullptr;
    QString open_error;
    std::vector<std::pair<QPointer<QObject>, std::function<void()>>> listeners;

    Vault() {
        QString path = qEnvironmentVariable("PTZ_VAULT_PATH");
        if (path.isEmpty()) {
            // ~/.var/app/org.ptzcontrol.app/data/PTZControl/logins.vault inside the Flatpak.
            QString folder = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/PTZControl";
            QDir().mkpath(folder);
            QFile::setPermissions(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
            path = folder + "/logins.vault";
        }
        char error[PTZ_TEXT] = "";
        handle = ptz_vault_open(QFile::encodeName(path).constData(), error, sizeof error);
        if (!handle) open_error = QString::fromUtf8(error);
    }

    void changed() {
        for (auto &[owner, fn] : listeners)
            if (owner) fn();
    }
};

Vault &vault() {
    static Vault v;
    return v;
}

ptz_store to_store(LoginStore s) { return s == LoginStore::Ssh ? PTZ_STORE_SSH : PTZ_STORE_CAMERA; }

bool call(QString *error, const std::function<bool(char *, size_t)> &fn) {
    if (!vault().handle) {
        if (error) *error = vault().open_error.isEmpty() ? "Saved logins are unavailable" : vault().open_error;
        return false;
    }
    char buffer[PTZ_TEXT] = "";
    bool ok = fn(buffer, sizeof buffer);
    if (!ok && error) *error = QString::fromUtf8(buffer);
    return ok;
}

void show_error(QWidget *parent, const QString &title, const QString &message) {
    QMessageBox box(QMessageBox::Warning, title, title, QMessageBox::Ok, parent);
    box.setInformativeText(message);
    box.exec();
}

bool confirm_plain_text(QWidget *parent) {
    QMessageBox box(QMessageBox::Warning, "Store passwords as plain text?", "Store passwords as plain text?",
                    QMessageBox::NoButton, parent);
    box.setInformativeText("Saved SSH and camera passwords will be readable by anyone who can read your files.");
    QPushButton *cancel = box.addButton("Cancel", QMessageBox::RejectRole);
    QPushButton *plain = box.addButton("Store as Plain Text", QMessageBox::AcceptRole);
    box.setDefaultButton(cancel);
    box.exec();
    return box.clickedButton() == plain;
}

// A password field with Show password, for the login form.
struct PasswordRow {
    QLineEdit *field;
    QCheckBox *show;
};

PasswordRow password_row(const QString &value, const QString &placeholder) {
    auto *field = new QLineEdit(value);
    field->setEchoMode(QLineEdit::Password);
    field->setPlaceholderText(placeholder);
    field->setFont(mono_font(13));
    field->setAccessibleName("Password");
    auto *show = new QCheckBox("Show password");
    show->setToolTip("Show password — Shows the saved password as text while this form is open.");
    QObject::connect(show, &QCheckBox::toggled, field,
                     [field](bool on) { field->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password); });
    return {field, show};
}

// The New and Edit form: username and password, with Show password.
class LoginForm : public QDialog {
public:
    LoginForm(QWidget *parent, const SavedLogin &login, LoginStore store, const QString &title) : QDialog(parent) {
        setWindowTitle(title);
        setMinimumWidth(400);
        bool ssh = store == LoginStore::Ssh;
        name_ = new QLineEdit(login.name);
        name_->setPlaceholderText(ssh ? "e.g. jump host" : "e.g. cam1");
        name_->setFont(mono_font(13));
        name_->setAccessibleName("Name");
        user_ = new QLineEdit(login.user);
        user_->setPlaceholderText(ssh ? "username" : "admin");
        user_->setFont(mono_font(13));
        user_->setAccessibleName("Username");
        PasswordRow pw = password_row(login.password, ssh ? "blank for keys / agent" : "camera password");
        password_ = pw.field;
        note_ = new QLabel(ssh ? "The password may stay blank for keys or ssh-agent." : "");
        note_->setObjectName("secondary");
        note_->setWordWrap(true);

        auto *grid = new QGridLayout;
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(6);
        auto add = [&](int row, const QString &name, QWidget *w) {
            auto *l = new QLabel(name);
            l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            grid->addWidget(l, row, 0);
            grid->addWidget(w, row, 1);
        };
        add(0, "Name", name_);
        add(1, "Username", user_);
        add(2, "Password", password_);
        grid->addWidget(pw.show, 3, 1);

        auto *buttons = new QDialogButtonBox;
        QPushButton *save = buttons->addButton("Save", QDialogButtonBox::AcceptRole);
        buttons->addButton("Cancel", QDialogButtonBox::RejectRole);
        save->setDefault(true);
        connect(buttons, &QDialogButtonBox::accepted, this, [this] { try_accept(); });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        auto *layout = new QVBoxLayout(this);
        layout->addLayout(grid);
        layout->addWidget(note_);
        layout->addWidget(buttons);
        name_->setFocus();
    }

    ~LoginForm() override {
        QString t = password_->text();
        t.fill(QChar(0));
        password_->clear();
    }

    SavedLogin result;

private:
    void try_accept() {
        QString name = name_->text().trimmed(), user = user_->text().trimmed();
        if (name.isEmpty() || user.isEmpty()) {
            note_->setText("A name and a username are required.");
            return;
        }
        result = {name, user, password_->text()};
        accept();
    }

    QLineEdit *name_, *user_, *password_;
    QLabel *note_;
};

// Manage Logins: SSH Logins and Camera Logins, each a list with New, Edit and Delete,
// the Administrator Password and Lock/Unlock.
class ManageLogins : public QDialog {
public:
    explicit ManageLogins(QWidget *parent) : QDialog(parent) {
        setWindowTitle("Manage Logins");
        setObjectName("manageLogins");
        resize(680, 420);
        setMinimumSize(580, 320);

        ssh_tab_ = new QPushButton("SSH Logins");
        camera_tab_ = new QPushButton("Camera Logins");
        ssh_tab_->setToolTip("Saved SSH usernames and passwords");
        camera_tab_->setToolTip("Saved camera usernames and passwords. The two lists are kept apart, so any SSH login "
                                "can be used with any camera login.");
        for (QPushButton *b : {ssh_tab_, camera_tab_}) {
            b->setObjectName("segment");
            b->setCheckable(true);
            b->setAutoExclusive(true);
            b->setFixedHeight(26);
        }
        ssh_tab_->setChecked(true);
        new_ = new QPushButton("New…");
        edit_ = new QPushButton("Edit…");
        delete_ = new QPushButton("Delete");
        lock_ = new QPushButton("Lock");
        admin_ = new QPushButton("Create Administrator Password…");
        done_ = new QPushButton("Done");
        new_->setToolTip("New login — Add a saved login to this list.");
        edit_->setToolTip("Edit login — Change the selected login (or double-click it).");
        delete_->setToolTip("Delete login — Remove the selected login after a confirmation (Delete).");
        admin_->setToolTip("Administrator Password — Encrypts every saved SSH and camera login. Change it, or leave "
                           "the new password blank to keep logins as plain text.");
        lock_->setToolTip("Lock or unlock the saved logins with the Administrator Password.");
        done_->setToolTip("Close Manage Logins (Esc).");

        table_ = new QTableWidget(0, 3);
        table_->setHorizontalHeaderLabels({"Name", "Username", "Password"});
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->verticalHeader()->hide();
        table_->horizontalHeader()->setStretchLastSection(true);
        table_->setAlternatingRowColors(true);
        table_->setAccessibleName("Saved logins");
        table_->setColumnWidth(0, 220);
        table_->setColumnWidth(1, 220);
        table_->installEventFilter(this);

        status_ = new QLabel;
        status_->setObjectName("secondary");

        auto *top = new QHBoxLayout;
        top->setSpacing(0);
        top->addWidget(ssh_tab_);
        top->addWidget(camera_tab_);
        top->addStretch(1);
        top->setSpacing(6);
        for (QPushButton *b : {new_, edit_, delete_}) top->addWidget(b);
        auto *bottom = new QHBoxLayout;
        bottom->addWidget(lock_);
        bottom->addWidget(admin_);
        bottom->addWidget(status_, 1);
        bottom->addWidget(done_);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(14, 14, 14, 14);
        layout->setSpacing(10);
        layout->addLayout(top);
        layout->addWidget(table_, 1);
        layout->addLayout(bottom);

        connect(ssh_tab_, &QPushButton::clicked, this, [this] { table_->clearSelection(); reload(); });
        connect(camera_tab_, &QPushButton::clicked, this, [this] { table_->clearSelection(); reload(); });
        connect(new_, &QPushButton::clicked, this, [this] { new_login(); });
        connect(edit_, &QPushButton::clicked, this, [this] { edit_login(); });
        connect(delete_, &QPushButton::clicked, this, [this] { delete_login(); });
        connect(lock_, &QPushButton::clicked, this, [this] {
            if (vault_state() == VaultState::Locked) vault_unlock_dialog(this);
            else vault_lock();
        });
        connect(admin_, &QPushButton::clicked, this, [this] { vault_change_admin_dialog(this); });
        connect(done_, &QPushButton::clicked, this, &QDialog::close);
        connect(table_, &QTableWidget::itemSelectionChanged, this, [this] { update_buttons(); });
        connect(table_, &QTableWidget::cellDoubleClicked, this, [this] { edit_login(); });
        vault_subscribe(this, [this] { reload(); });
        reload();
    }

    void show_store(LoginStore store) {
        if (this->store() != store) table_->clearSelection();
        (store == LoginStore::Ssh ? ssh_tab_ : camera_tab_)->setChecked(true);
        reload();
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (object == table_ && event->type() == QEvent::KeyPress) {
            int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Delete || key == Qt::Key_Backspace) {
                delete_login();
                return true;
            }
        }
        return QDialog::eventFilter(object, event);
    }

private:
    LoginStore store() const { return camera_tab_->isChecked() ? LoginStore::Camera : LoginStore::Ssh; }

    int selected() const {
        auto rows = table_->selectionModel()->selectedRows();
        return rows.isEmpty() ? -1 : rows.first().row();
    }

    void reload() {
        int keep = selected();
        logins_ = vault_logins(store());
        table_->setRowCount(static_cast<int>(logins_.size()));
        for (int r = 0; r < static_cast<int>(logins_.size()); ++r) {
            const SavedLogin &l = logins_[r];
            auto *name = new QTableWidgetItem(l.name), *user = new QTableWidgetItem(l.user);
            name->setFont(mono_font(12));
            user->setFont(mono_font(12));
            table_->setItem(r, 0, name);
            table_->setItem(r, 1, user);
            table_->setItem(r, 2, new QTableWidgetItem(l.password.isEmpty() ? "—" : "••••••"));
        }
        if (keep >= 0 && keep < table_->rowCount()) table_->selectRow(keep);
        VaultState state = vault_state();
        admin_->setText(state == VaultState::Locked || state == VaultState::Unlocked ? "Change Administrator Password…"
                                                                                     : "Create Administrator Password…");
        lock_->setText(state == VaultState::Locked ? "Unlock…" : "Lock");
        lock_->setAccessibleName(state == VaultState::Locked ? "Unlock Saved Logins" : "Lock Saved Logins");
        lock_->setEnabled(state == VaultState::Locked || state == VaultState::Unlocked);
        status_->setText(state == VaultState::Locked     ? "Locked."
                         : state == VaultState::Unlocked ? "Encrypted with the Administrator Password."
                         : state == VaultState::Plain    ? "Stored as plain text."
                         : state == VaultState::New      ? "No logins saved yet."
                                                         : vault_open_error());
        update_buttons();
    }

    void update_buttons() {
        VaultState state = vault_state();
        bool usable = state == VaultState::Plain || state == VaultState::Unlocked || state == VaultState::New;
        new_->setEnabled(usable);
        edit_->setEnabled(usable && selected() >= 0);
        delete_->setEnabled(usable && selected() >= 0);
    }

    void new_login() {
        if (!vault_ensure_writable(this)) return;
        LoginForm form(this, SavedLogin{}, store(), store() == LoginStore::Ssh ? "New SSH Login" : "New Camera Login");
        if (form.exec() == QDialog::Accepted) save(form.result, -1);
    }

    void edit_login() {
        int row = selected();
        if (row < 0 || row >= static_cast<int>(logins_.size()) || !vault_ensure_writable(this)) return;
        LoginForm form(this, logins_[row], store(), store() == LoginStore::Ssh ? "Edit SSH Login" : "Edit Camera Login");
        if (form.exec() == QDialog::Accepted) save(form.result, row);
    }

    void save(const SavedLogin &login, int row) {
        QString error;
        int index = -1;
        if (!vault_save(login, store(), row, &error, &index)) {
            show_error(this, "Can't save the login", error);
            return;
        }
        table_->selectRow(index);
    }

    void delete_login() {
        int row = selected();
        VaultState state = vault_state();
        if (row < 0 || row >= static_cast<int>(logins_.size()) ||
            (state != VaultState::Plain && state != VaultState::Unlocked))
            return;
        QMessageBox box(QMessageBox::Warning, "Delete login", "Delete the login " + logins_[row].name + "?",
                        QMessageBox::NoButton, this);
        box.setInformativeText(QString("This removes it from the saved ") +
                               (store() == LoginStore::Ssh ? "SSH" : "camera") + " logins.");
        QPushButton *cancel = box.addButton("Cancel", QMessageBox::RejectRole);
        QPushButton *remove = box.addButton("Delete", QMessageBox::DestructiveRole);
        box.setDefaultButton(cancel);
        box.exec();
        if (box.clickedButton() != remove) return;
        QString error;
        if (!vault_remove(row, store(), &error)) show_error(this, "Can't delete the login", error);
    }

    QPushButton *ssh_tab_, *camera_tab_, *new_, *edit_, *delete_, *lock_, *admin_, *done_;
    QTableWidget *table_;
    QLabel *status_;
    std::vector<SavedLogin> logins_;
};

QPointer<ManageLogins> g_manage;

}  // namespace

VaultState vault_state() {
    if (!vault().handle) return VaultState::Unavailable;
    switch (ptz_vault_get_state(vault().handle)) {
    case PTZ_VAULT_PLAIN: return VaultState::Plain;
    case PTZ_VAULT_LOCKED: return VaultState::Locked;
    case PTZ_VAULT_UNLOCKED: return VaultState::Unlocked;
    default: return VaultState::New;
    }
}

QString vault_open_error() { return vault().open_error; }

std::vector<SavedLogin> vault_logins(LoginStore store) {
    std::vector<SavedLogin> out;
    if (!vault().handle) return out;
    size_t count = ptz_vault_count(vault().handle, to_store(store));
    for (size_t i = 0; i < count; ++i) {
        ptz_login l{};
        if (!ptz_vault_login(vault().handle, to_store(store), i, &l)) continue;
        out.push_back({QString::fromUtf8(l.name), QString::fromUtf8(l.user), QString::fromUtf8(l.password)});
    }
    return out;
}

bool vault_save(const SavedLogin &login, LoginStore store, int replacing, QString *error, int *index) {
    QByteArray name = login.name.toUtf8(), user = login.user.toUtf8(), password = login.password.toUtf8();
    long result = -1;
    bool ok = call(error, [&](char *buffer, size_t size) {
        ptz_login l{name.constData(), user.constData(), password.constData()};
        result = ptz_vault_save(vault().handle, to_store(store), replacing, &l, buffer, size);
        return result >= 0;
    });
    password.fill('\0');
    if (ok) {
        if (index) *index = static_cast<int>(result);
        vault().changed();
    }
    return ok;
}

bool vault_remove(int index, LoginStore store, QString *error) {
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

void vault_subscribe(QObject *owner, std::function<void()> fn) { vault().listeners.push_back({owner, std::move(fn)}); }

QString vault_ask_name(QWidget *parent, LoginStore store, const QString &user, const QString &suggested) {
    bool ssh = store == LoginStore::Ssh;
    QString note = QString("Name the ") + (ssh ? "SSH" : "camera") + " login for " + user +
                   ", to find it in the Login menu. A saved login with the same name is updated.";
    QString name = suggested;
    while (true) {
        QDialog dialog(parent);
        dialog.setWindowTitle(ssh ? "Save SSH Login" : "Save Camera Login");
        auto *text = new QLabel(note);
        text->setWordWrap(true);
        auto *field = new QLineEdit(name);
        field->setPlaceholderText(ssh ? "e.g. jump host" : "e.g. cam1");
        field->setAccessibleName("Login name");
        auto *buttons = new QDialogButtonBox;
        buttons->addButton("Save", QDialogButtonBox::AcceptRole)->setDefault(true);
        buttons->addButton("Don't Save", QDialogButtonBox::RejectRole);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        auto *layout = new QVBoxLayout(&dialog);
        layout->addWidget(text);
        layout->addWidget(field);
        layout->addWidget(buttons);
        dialog.setMinimumWidth(420);
        field->selectAll();
        if (dialog.exec() != QDialog::Accepted) return {};
        name = field->text().trimmed();
        if (!name.isEmpty()) return name;
        note = "Enter a name, or Don't Save.";
    }
}

bool vault_unlock_dialog(QWidget *parent) {
    QString note = "Enter the Administrator Password to use your saved SSH and camera logins.";
    while (vault_state() == VaultState::Locked) {
        QDialog dialog(parent);
        dialog.setWindowTitle("Unlock Saved Logins");
        auto *text = new QLabel(note);
        text->setWordWrap(true);
        auto *field = new QLineEdit;
        field->setEchoMode(QLineEdit::Password);
        field->setAccessibleName("Administrator Password");
        auto *buttons = new QDialogButtonBox;
        buttons->addButton("Unlock", QDialogButtonBox::AcceptRole)->setDefault(true);
        buttons->addButton("Cancel", QDialogButtonBox::RejectRole);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        auto *layout = new QVBoxLayout(&dialog);
        layout->addWidget(text);
        layout->addWidget(field);
        layout->addWidget(buttons);
        dialog.setMinimumWidth(420);
        bool ok = dialog.exec() == QDialog::Accepted;
        QByteArray answer = field->text().toUtf8();
        field->clear();
        if (!ok) return false;
        QString error;
        if (call(&error, [&](char *buffer, size_t size) {
                return ptz_vault_unlock(vault().handle, answer.constData(), buffer, size) != 0;
            }))
            vault().changed();
        else
            note = error + ". Try again.";
        answer.fill('\0');
    }
    return true;
}

bool vault_change_admin_dialog(QWidget *parent) {
    if (vault_state() == VaultState::Locked && !vault_unlock_dialog(parent)) return false;
    if (vault_state() == VaultState::Unavailable) return false;
    bool changing = vault_state() == VaultState::Unlocked;
    QString note = changing ? "Every saved login is encrypted again with the new password. Leave both fields blank to "
                              "remove the Administrator Password."
                            : "It encrypts your saved SSH and camera logins. It can't be recovered if you forget it.";
    while (true) {
        QDialog dialog(parent);
        dialog.setWindowTitle(changing ? "Change Administrator Password" : "Create Administrator Password");
        auto *text = new QLabel(note);
        text->setWordWrap(true);
        auto *first = new QLineEdit, *second = new QLineEdit;
        first->setPlaceholderText("New Administrator Password");
        second->setPlaceholderText("Repeat it");
        first->setAccessibleName("New Administrator Password");
        second->setAccessibleName("Repeat the new Administrator Password");
        for (QLineEdit *f : {first, second}) f->setEchoMode(QLineEdit::Password);
        auto *buttons = new QDialogButtonBox;
        buttons->addButton(changing ? "Change" : "Create", QDialogButtonBox::AcceptRole)->setDefault(true);
        buttons->addButton("Cancel", QDialogButtonBox::RejectRole);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        auto *layout = new QVBoxLayout(&dialog);
        layout->addWidget(text);
        layout->addWidget(first);
        layout->addWidget(second);
        layout->addWidget(buttons);
        dialog.setMinimumWidth(440);
        bool ok = dialog.exec() == QDialog::Accepted;
        QByteArray a = first->text().toUtf8(), b = second->text().toUtf8();
        first->clear();
        second->clear();
        if (!ok) return false;
        if (a != b) {
            note = "The two passwords don't match. Try again.";
            continue;
        }
        if (a.isEmpty()) {
            if (!changing) {
                note = "Enter a password, or Cancel.";
                continue;
            }
            if (!confirm_plain_text(parent)) return false;
        }
        QString error;
        bool done = call(&error, [&](char *buffer, size_t size) {
            return ptz_vault_set_admin_password(vault().handle, a.constData(), buffer, size) != 0;
        });
        a.fill('\0');
        b.fill('\0');
        if (!done) {
            show_error(parent, "Can't change the Administrator Password", error);
            return false;
        }
        vault().changed();
        return true;
    }
}

static bool choose_storage(QWidget *parent) {
    QMessageBox box(QMessageBox::Information, "Protect Saved Logins", "Protect Saved Logins", QMessageBox::NoButton, parent);
    box.setInformativeText(
        "Saved logins hold passwords. An Administrator Password encrypts your saved SSH and camera logins; you enter it "
        "once each time PTZ Control starts. It can't be recovered if you forget it.\n\n"
        "Without one, passwords are stored as plain text, which anyone who can read your files can see.");
    QPushButton *create = box.addButton("Create Administrator Password…", QMessageBox::AcceptRole);
    box.addButton("Not Now", QMessageBox::RejectRole);
    QPushButton *plain = box.addButton("Store as Plain Text…", QMessageBox::DestructiveRole);
    box.setDefaultButton(create);
    box.exec();
    if (box.clickedButton() == create) return vault_change_admin_dialog(parent);
    if (box.clickedButton() != plain || !confirm_plain_text(parent)) return false;
    QString error;
    if (!call(&error, [](char *buffer, size_t size) {
            return ptz_vault_set_admin_password(vault().handle, nullptr, buffer, size) != 0;
        })) {
        show_error(parent, "Can't save logins", error);
        return false;
    }
    vault().changed();
    return true;
}

bool vault_ensure_readable(QWidget *parent) {
    switch (vault_state()) {
    case VaultState::Locked: return vault_unlock_dialog(parent);
    case VaultState::Unavailable: show_error(parent, "Saved logins are unavailable", vault_open_error()); return false;
    default: return true;
    }
}

bool vault_ensure_writable(QWidget *parent) {
    if (vault_state() == VaultState::New) return choose_storage(parent);
    return vault_ensure_readable(parent);
}

void show_manage_logins(QWidget *parent, std::optional<LoginStore> store) {
    if (vault_state() == VaultState::Unavailable) {
        show_error(parent, "Saved logins are unavailable", vault_open_error());
        return;
    }
    if (vault_state() == VaultState::Locked) vault_unlock_dialog(parent);
    if (!g_manage) {
        g_manage = new ManageLogins(parent);
        g_manage->setAttribute(Qt::WA_DeleteOnClose);
    }
    if (store) g_manage->show_store(*store);
    g_manage->show();
    g_manage->raise();
    g_manage->activateWindow();
}

QPixmap manage_logins_snapshot(QWidget *parent, LoginStore store) {
    ManageLogins dialog(parent);
    dialog.show_store(store);
    dialog.show();
    QPixmap picture = dialog.grab();
    dialog.close();
    return picture;
}

QPixmap login_form_snapshot(QWidget *parent, LoginStore store) {
    std::vector<SavedLogin> logins = vault_logins(store);
    if (logins.empty()) return {};
    LoginForm form(parent, logins.front(), store, "Edit Camera Login");
    form.show();
    QPixmap picture = form.grab();
    form.close();
    return picture;
}
