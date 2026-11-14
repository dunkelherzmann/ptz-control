// The console window: link configuration, live instruments, target entry and
// the event buffer. Logic mirrors the macOS and Windows consoles.
#include "app.h"
#include "instruments.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QDialog>
#include <QDesktopServices>
#include <QDir>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QShortcut>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <cmath>
#include <deque>
#include <QPointer>
#include <QTextEdit>
#include <QFile>

namespace {

Theme g_theme = make_theme(Phosphor::System);

enum class State { Idle, Connecting, Connected, Disconnecting };

QString describe(const Snapshot &s) {
    auto v = [](const std::optional<Reading> &r) { return r ? r->text : QString("unknown"); };
    QString text = "Pan:" + v(s.pan) + " Tilt:" + v(s.tilt) + " Zoom:" + v(s.zoom);
    if (!s.move_status.isEmpty()) text += " (" + s.move_status + ")";
    return text;
}

QString css(const QColor &c) { return c.name(QColor::HexRgb); }

// A rounded instrument panel with a small monospaced caption, and optional controls at the caption row's right end.
class Panel : public QWidget {
public:
    Panel(const QString &caption, QWidget *parent = nullptr) : QWidget(parent), caption_(caption) {}
    void set_caption(const QString &caption) {
        caption_ = caption;
        update();
    }
    void set_accessory(QWidget *accessory) {
        accessory_ = accessory;
        accessory->setParent(this);
        place_accessory();
    }

protected:
    void resizeEvent(QResizeEvent *) override { place_accessory(); }
    void place_accessory() {
        if (!accessory_) return;
        accessory_->adjustSize();
        accessory_->move(width() - accessory_->width() - 8, 4);
    }

    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(g_theme.border, 1));
        p.setBrush(g_theme.panel);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
        p.setFont(mono_font(10, true));
        p.setPen(g_theme.secondary);
        QFontMetrics m(p.font());
        int room = width() - 24 - (accessory_ ? accessory_->width() + 10 : 0);
        p.drawText(QPoint(12, 8 + m.ascent()), m.elidedText(caption_, Qt::ElideRight, room));
    }

private:
    QString caption_;
    QWidget *accessory_ = nullptr;
};

// The ⟳ Refresh Picture button: a circular arrow, framed only while the pointer is over it.
class RefreshButton : public QAbstractButton {
public:
    RefreshButton(QWidget *parent = nullptr) : QAbstractButton(parent) {
        setFixedSize(24, 20);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QColor ink = isEnabled() ? g_theme.text : blend(g_theme.panel, g_theme.text, 0.4);
        if (isEnabled() && underMouse()) {
            p.setPen(QPen(g_theme.border, 1));
            p.setBrush(isDown() ? blend(g_theme.panel, g_theme.text, 0.12) : g_theme.button);
            p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
        }
        QPointF c = QRectF(rect()).center();
        double radius = 5.5;
        QPen pen(ink, 1.6);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        // Clockwise arc from the top, open at the upper right, with the arrowhead at its end.
        p.drawArc(QRectF(c.x() - radius, c.y() - radius, 2 * radius, 2 * radius), 60 * 16, 300 * 16);
        QPointF tip(c.x() + radius * 0.5, c.y() - radius * 0.8660254);  // the arc's end, at 60 degrees
        QPointF along(0.8660254, 0.5), across(-0.5, 0.8660254);         // clockwise there, and square to it
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        p.drawPolygon(QPolygonF({tip + along * 3.2, tip - along * 1.2 + across * 3, tip - along * 1.2 - across * 3}));
    }
};

// Status pill in the header.
class Chip : public QWidget {
public:
    Chip(QWidget *parent = nullptr) : QWidget(parent) { setFixedHeight(24); }
    void set(const QString &text, const QColor &color) {
        text_ = "● " + text;
        color_ = color;
        setFixedWidth(QFontMetrics(mono_font(11, true)).horizontalAdvance(text_) + 22);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QColor stroke = color_;
        stroke.setAlpha(140);
        p.setPen(QPen(stroke, 1));
        p.setBrush(Qt::NoBrush);
        QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
        p.setFont(mono_font(11, true));
        p.setPen(color_);
        p.drawText(rect(), Qt::AlignCenter, text_);
    }

private:
    QString text_;
    QColor color_;
};

// Current coordinates: the camera's own digits, or an em dash when unavailable.
class Readouts : public QWidget {
public:
    struct Item {
        QString label;
        std::optional<QString> value;
        QString detail;
    };
    std::vector<Item> items;
    QString starting;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        double y = 0, w = width();
        auto line = [&](const QString &text, double size, const QColor &color, bool bold, double top) {
            p.setFont(mono_font(size, bold));
            p.setPen(color);
            QFontMetrics m(p.font());
            p.drawText(QPointF(0, top + m.ascent()), m.elidedText(text, Qt::ElideRight, static_cast<int>(w)));
        };
        for (const auto &item : items) {
            line(item.label, 10, g_theme.secondary, true, y);
            line(item.value.value_or("—"), 22, item.value ? g_theme.primary : g_theme.secondary, true, y + 14);
            line(item.detail, 10, g_theme.secondary, false, y + 44);
            y += 66;
        }
        p.setFont(mono_font(11));
        p.setPen(g_theme.start);
        p.drawText(QRectF(0, y, w, std::max(14.0, height() - y)), Qt::TextWordWrap, "Starting: " + starting);
    }
};

struct Target {
    std::optional<double> pan, tilt, zoom;
    bool empty() const { return !pan && !tilt && !zoom; }
};

class Console : public QWidget {
public:
    Console() {
        settings_ = Settings::load();
        phosphor_ = static_cast<Phosphor>(settings_.theme);
        route_ = settings_.route;
        vendor_ = settings_.vendor;
        link_ = create_link();
        setObjectName("console");
        setWindowTitle("PTZ Control");
        setMinimumSize(1000, 740);
        resize(1180, 820);
        build();
        wire();
        apply_theme();
        update_controls();
        set_plane_mode(settings_.plane_mode == "picture" ? PlaneMode::Picture : PlaneMode::Range);
        log_line("PTZ Control ready · core " + QString::fromUtf8(ptz_core_version()));
        if (vault_state() == VaultState::Unavailable) log_line("Saved logins are unavailable: " + vault_open_error(), true);
        auto *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] { tick(); });
        timer->start(250);
        if (qEnvironmentVariableIsSet("PTZ_SNAPSHOT_PATH")) QTimer::singleShot(300, this, [this] { run_snapshot_script(); });
    }

protected:
    void showEvent(QShowEvent *) override {
        if (plane_->state.mode == PlaneMode::Picture) QTimer::singleShot(0, this, [this] { fit_window_to_picture(); });
    }

    void closeEvent(QCloseEvent *e) override {
        if (state_ == State::Connected || state_ == State::Connecting) {
            e->ignore();
            if (!closing_) {
                closing_ = true;
                disconnect([this] { close(); });
            }
            return;
        }
        for (QLineEdit *p : {cam_password_, ssh_password_}) {
            QString t = p->text();
            t.fill(QChar(0));
            p->clear();
        }
        if (link_) {
            destroy_link(link_);
            link_ = nullptr;
        }
        e->accept();
    }

private:
    // ---- construction -------------------------------------------------------------

    QLabel *label(const QString &text, const char *name = nullptr) {
        auto *l = new QLabel(text);
        if (name) l->setObjectName(name);
        return l;
    }

    QLabel *caption(const QString &text) {
        auto *l = label(text, "caption");
        l->setFont(mono_font(10, true));
        return l;
    }

    QPushButton *button(const QString &text, const char *name = nullptr) {
        auto *b = new QPushButton(text);
        if (name) b->setObjectName(name);
        b->setCursor(Qt::PointingHandCursor);
        return b;
    }

    QLineEdit *edit(const QString &value, const QString &placeholder = {}, bool secret = false) {
        auto *e = new QLineEdit(value);
        e->setPlaceholderText(placeholder);
        e->setFont(mono_font(13));
        if (secret) e->setEchoMode(QLineEdit::Password);
        return e;
    }

    void build() {
        const Settings &s = settings_;
        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(12, 12, 12, 6);
        root->setSpacing(6);
        auto *body = new QHBoxLayout;
        body->setSpacing(12);
        root->addLayout(body, 1);
        footer_ = label("", "secondary");
        footer_->setFont(mono_font(10));
        root->addWidget(footer_);

        // Sidebar.
        auto *sidebar = new Panel("SECURE LINK CONFIGURATION");
        sidebar->setFixedWidth(300);
        body->addWidget(sidebar);
        auto *side = new QVBoxLayout(sidebar);
        side->setContentsMargins(12, 30, 12, 10);
        side->setSpacing(4);
        auto segments = [&](QPushButton *a, QPushButton *b) {
            auto *row = new QHBoxLayout;
            row->setSpacing(0);
            auto *group = new QButtonGroup(this);  // each pair is exclusive on its own
            for (QPushButton *p : {a, b}) {
                p->setCheckable(true);
                group->addButton(p);
                p->setFixedHeight(26);
                row->addWidget(p);
            }
            return row;
        };
        route_local_ = button("Local", "segment");
        route_ssh_ = button("SSH Host", "segment");
        vendor_onvif_ = button("Bosch/Milesight", "segment");
        vendor_axis_ = button("Axis", "segment");
        side->addWidget(caption("ROUTE"));
        side->addLayout(segments(route_local_, route_ssh_));
        side->addSpacing(6);
        side->addWidget(caption("CAMERA VENDOR"));
        side->addLayout(segments(vendor_onvif_, vendor_axis_));
        vendor_note_ = label("", "secondary");
        vendor_note_->setWordWrap(true);
        vendor_note_->setMinimumHeight(32);
        side->addWidget(vendor_note_);

        ssh_host_ = edit(s.ssh_host, "jump.example.com");
        ssh_user_ = edit(s.ssh_user, "username");
        ssh_password_ = edit("", "blank for keys / agent", true);
        ssh_port_ = edit(QString::number(s.ssh_port));
        ssh_key_ = edit(s.ssh_identity, "optional, e.g. ~/.ssh/id_ed25519");
        const char *env_password = std::getenv("PTZ_PASSWORD");
        cam_host_ = edit(s.camera_host, "192.168.1.100");
        cam_port_ = edit(QString::number(s.camera_port));
        cam_user_ = edit(s.camera_user);
        cam_password_ = edit(env_password ? QString::fromUtf8(env_password) : QString(), "required", true);
        https_port_ = edit(QString::number(s.https_port));
        profile_ = edit(QString::number(s.profile));
        timeout_ = edit(format_value(s.timeout));

        auto *grid = new QGridLayout;
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(5);
        grid->setColumnMinimumWidth(0, 80);
        int row = 0;
        auto section = [&](const QString &text, bool ssh) {
            QLabel *c = caption(text);
            if (row) grid->setRowMinimumHeight(row++, 4);
            grid->addWidget(c, row++, 0, 1, 2);
            if (ssh) ssh_widgets_.push_back(c);
        };
        auto field_row = [&](const QString &name, QLineEdit *e, bool ssh) {
            QLabel *l = label(name);
            l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            grid->addWidget(l, row, 0);
            grid->addWidget(e, row++, 1);
            fields_.push_back(e);
            if (ssh) {
                ssh_widgets_.push_back(l);
                ssh_widgets_.push_back(e);
            }
        };
        auto widget_row = [&](const QString &name, QWidget *w, bool ssh) {
            QLabel *l = label(name);
            l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            grid->addWidget(l, row, 0);
            grid->addWidget(w, row++, 1);
            if (ssh) {
                ssh_widgets_.push_back(l);
                ssh_widgets_.push_back(w);
            }
        };
        ssh_login_ = new QComboBox;
        cam_login_ = new QComboBox;
        save_ssh_ = new QCheckBox("Save login");
        save_cam_ = new QCheckBox("Save login");
        ssh_login_->setToolTip("SSH login — Fill the SSH username and password from a saved SSH login, or open Manage "
                               "Logins (Ctrl+Shift+L).");
        cam_login_->setToolTip("Camera login — Fill the camera username and password from a saved camera login, or open "
                               "Manage Logins (Ctrl+Shift+L).");
        ssh_login_->setAccessibleName("Saved SSH login");
        cam_login_->setAccessibleName("Saved camera login");
        save_ssh_->setToolTip("Save login — Once connected, save this SSH username and password to the saved SSH logins.");
        save_cam_->setToolTip("Save login — Once connected, save this camera username and password to the saved camera "
                              "logins.");
        save_ssh_->setAccessibleName("Save this SSH login");
        save_cam_->setAccessibleName("Save this camera login");
        section("SSH HOST", true);
        widget_row("Login", ssh_login_, true);
        field_row("Host", ssh_host_, true);
        field_row("Username", ssh_user_, true);
        field_row("Password", ssh_password_, true);
        field_row("Port", ssh_port_, true);
        field_row("Key file", ssh_key_, true);
        widget_row("", save_ssh_, true);
        section("CAMERA", false);
        widget_row("Login", cam_login_, false);
        field_row("Address", cam_host_, false);
        field_row("HTTP port", cam_port_, false);
        field_row("Username", cam_user_, false);
        field_row("Password", cam_password_, false);
        widget_row("", save_cam_, false);
        section("ADVANCED", false);
        field_row("HTTPS port", https_port_, false);
        field_row("Profile", profile_, false);
        field_row("Timeout (s)", timeout_, false);
        side->addLayout(grid);
        side->addSpacing(8);
        connect_ = button("Connect", "primary");
        connect_->setFixedHeight(32);
        side->addWidget(connect_);
        message_ = label("");
        message_->setWordWrap(true);
        message_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        side->addWidget(message_);
        side->addStretch(1);

        // Right side.
        auto *right = new QVBoxLayout;
        right->setSpacing(12);
        body->addLayout(right, 1);

        auto *header = new QHBoxLayout;
        header->setContentsMargins(4, 0, 0, 0);
        auto *titles = new QVBoxLayout;
        titles->setSpacing(2);
        title_ = label("", "title");
        units_ = label("", "secondary");
        titles->addWidget(title_);
        titles->addWidget(units_);
        header->addLayout(titles, 1);
        chip_ = new Chip;
        header->addWidget(chip_, 0, Qt::AlignTop);
        header->addSpacing(10);
        menu_button_ = main_menu_button();
        header->addWidget(menu_button_, 0, Qt::AlignTop);
        right->addLayout(header);

        auto *middle = new QHBoxLayout;
        middle->setSpacing(12);
        auto *instruments = new QVBoxLayout;
        instruments->setSpacing(12);
        auto *plane_panel = plane_panel_ = new Panel(kRangeCaption);
        plane_ = new PlaneView;
        auto *accessory = new QWidget;
        accessory->setAccessibleName("Position plane view");
        auto *switch_row = new QHBoxLayout(accessory);
        switch_row->setContentsMargins(0, 0, 0, 0);
        switch_row->setSpacing(0);
        refresh_picture_ = new RefreshButton;
        refresh_picture_->setToolTip("Refresh Picture: take a new picture of the camera's view now (Ctrl+Shift+R)");
        refresh_picture_->setAccessibleName("Refresh Picture");
        switch_row->addWidget(refresh_picture_);
        switch_row->addSpacing(6);
        show_range_ = button("Range", "mini");
        show_picture_ = button("Picture", "mini");
        show_range_->setToolTip("Range: the camera's whole pan/tilt range. Click to aim, double-click to move (Ctrl+1)");
        show_picture_->setToolTip("Picture: a still picture of what the camera sees. Click a point to aim at it, "
                                  "double-click to move the camera there (Ctrl+2)");
        auto *mode_group = new QButtonGroup(this);
        for (QPushButton *b : {show_range_, show_picture_}) {
            b->setCheckable(true);
            b->setFocusPolicy(Qt::NoFocus);
            b->setFixedHeight(20);
            mode_group->addButton(b);
            switch_row->addWidget(b);
        }
        plane_panel->set_accessory(accessory);
        (new QVBoxLayout(plane_panel))->addWidget(plane_);
        instruments->addWidget(plane_panel, 1);
        auto *meters_panel = new Panel("AXIS TRAVEL // % OF ADVERTISED LIMITS — DRAG TO AIM, DOUBLE-CLICK TO MOVE");
        meters_panel->setFixedHeight(150);
        auto *meters = new QVBoxLayout(meters_panel);
        meters->setSpacing(0);
        meter_pan_ = new MeterView("PAN");
        meter_tilt_ = new MeterView("TILT");
        meter_zoom_ = new MeterView("ZOOM");
        for (MeterView *m : {meter_pan_, meter_tilt_, meter_zoom_}) meters->addWidget(m);
        instruments->addWidget(meters_panel);
        middle->addLayout(instruments, 1);
        auto *readout_panel = new Panel("CURRENT COORDINATES");
        readout_panel->setFixedWidth(230);
        readouts_ = new Readouts;
        (new QVBoxLayout(readout_panel))->addWidget(readouts_);
        middle->addWidget(readout_panel);
        right->addLayout(middle, 1);

        auto *target_panel = new Panel("TARGET // ABSOLUTE MOVE");
        target_panel->setFixedHeight(84);
        auto *target = new QVBoxLayout(target_panel);
        target->setSpacing(6);
        auto *target_row = new QHBoxLayout;
        target_row->setSpacing(8);
        target_pan_ = edit("", "Pan: unchanged");
        target_tilt_ = edit("", "Tilt: unchanged");
        target_zoom_ = edit("", "Zoom: unchanged");
        for (QLineEdit *f : {target_pan_, target_tilt_, target_zoom_}) {
            f->setFixedWidth(130);
            target_row->addWidget(f);
        }
        move_ = button("Move", "primary");
        start_ = button("Return to Start");
        stop_ = button("Stop", "stop");
        web_ = button("Web Interface");
        target_row->addWidget(move_);
        target_row->addStretch(1);
        for (QPushButton *b : {start_, stop_, web_}) target_row->addWidget(b);
        target->addLayout(target_row);
        target->addWidget(label("Absolute targets, not increments. Leave a field blank (or x) to keep that axis.", "secondary"));
        right->addWidget(target_panel);

        auto *log_panel = new Panel("EVENT BUFFER // SESSION");
        log_panel->setFixedHeight(150);
        log_ = new QPlainTextEdit;
        log_->setReadOnly(true);
        log_->setFont(mono_font(12));
        log_->setFrameShape(QFrame::NoFrame);
        log_->setMaximumBlockCount(500);
        (new QVBoxLayout(log_panel))->addWidget(log_);
        right->addWidget(log_panel);

        for (auto *panel : {plane_panel, meters_panel, readout_panel, target_panel, log_panel})
            panel->layout()->setContentsMargins(12, 28, 12, 10);
        setTabOrder(cam_password_, connect_);
    }

    void wire() {
        connect(route_local_, &QPushButton::clicked, this, [this] { set_route(0); });
        connect(route_ssh_, &QPushButton::clicked, this, [this] { set_route(1); });
        connect(vendor_onvif_, &QPushButton::clicked, this, [this] { set_vendor(0); });
        connect(vendor_axis_, &QPushButton::clicked, this, [this] { set_vendor(1); });
        connect(connect_, &QPushButton::clicked, this, [this] { toggle_connection(); });
        connect(move_, &QPushButton::clicked, this, [this] { send_move(); });
        connect(start_, &QPushButton::clicked, this, [this] { return_to_start(); });
        connect(stop_, &QPushButton::clicked, this, [this] { stop_motion(); });
        connect(web_, &QPushButton::clicked, this, [this] { open_web(); });
        connect(show_range_, &QPushButton::clicked, this, [this] { set_plane_mode(PlaneMode::Range); });
        connect(show_picture_, &QPushButton::clicked, this, [this] { set_plane_mode(PlaneMode::Picture); });
        connect(refresh_picture_, &QAbstractButton::clicked, this, [this] { refresh_picture(); });
        for (QLineEdit *f : fields_)
            connect(f, &QLineEdit::returnPressed, this, [this] {
                if (state_ == State::Idle) connect_camera();
            });
        for (QLineEdit *f : {ssh_user_, ssh_password_})
            connect(f, &QLineEdit::textEdited, this, [this] { ssh_login_->setCurrentIndex(0); });
        for (QLineEdit *f : {cam_user_, cam_password_})
            connect(f, &QLineEdit::textEdited, this, [this] { cam_login_->setCurrentIndex(0); });
        for (QComboBox *box : {ssh_login_, cam_login_})
            connect(box, &QComboBox::activated, this, [this, box](int index) { login_chosen(box, index); });
        reload_logins();
        vault_subscribe(this, [this] { reload_logins(); });
        for (QLineEdit *f : {target_pan_, target_tilt_, target_zoom_}) {
            connect(f, &QLineEdit::textChanged, this, [this] { update_target_markers(); });
            connect(f, &QLineEdit::textEdited, this, [this] {  // an absolute target takes over from a point of the picture
                plane_->state.picture_target.reset();
                plane_->update();
            });
            connect(f, &QLineEdit::returnPressed, this, [this] { send_move(); });
        }

        plane_->state.on_aim = [this](double pan, double tilt) {
            target_pan_->setText(format_value(pan));
            target_tilt_->setText(format_value(tilt));
        };
        plane_->state.on_aim_and_move = [this](double pan, double tilt) {
            target_pan_->setText(format_value(pan));
            target_tilt_->setText(format_value(tilt));
            send_move();
        };
        plane_->state.on_picture_aim = [this](QPointF point) { aim_in_picture(point); };
        plane_->state.on_picture_aim_and_move = [this](QPointF point) {
            aim_in_picture(point);
            send_move();
        };
        for (auto [meter, field] : {std::pair{meter_pan_, target_pan_}, std::pair{meter_tilt_, target_tilt_},
                                    std::pair{meter_zoom_, target_zoom_}}) {
            QLineEdit *f = field;
            meter->state.on_aim = [f](double v) { f->setText(format_value(v)); };
            meter->state.on_aim_and_move = [this, f](double v) {
                f->setText(format_value(v));
                send_move();
            };
        }

        install_shortcuts(this);

        link_set_delegate(link_, LinkDelegate{
            [this](const QString &line) { log_line(line); },
            [this](const QString &message, bool secret) { return run_prompt(message, secret); },
            [this](const QString &title, const QString &message) {
                return QMessageBox::warning(this, title, message, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) ==
                       QMessageBox::Yes;
            },
        });
    }

    // ---- event buffer ---------------------------------------------------------------

    void log_line(const QString &line, bool warning = false) {
        QString text = QDateTime::currentDateTime().toString("HH:mm:ss") + "  " + line;
        log_lines_.push_back({text, warning});
        if (log_lines_.size() > 500) log_lines_.pop_front();
        append_log(text, warning);
    }

    void append_log(const QString &text, bool warning) {
        QTextCharFormat format;
        format.setForeground(warning ? g_theme.warning : g_theme.text);
        QTextCursor cursor(log_->document());
        cursor.movePosition(QTextCursor::End);
        if (!log_->document()->isEmpty()) cursor.insertBlock();
        cursor.insertText(text, format);
        log_->verticalScrollBar()->setValue(log_->verticalScrollBar()->maximum());
    }

    // ---- prompts --------------------------------------------------------------------

    std::optional<QString> run_prompt(const QString &message, bool secret) {
        QInputDialog dialog(this);
        dialog.setWindowTitle("SSH needs your input");
        dialog.setLabelText(message);
        dialog.setTextEchoMode(secret ? QLineEdit::Password : QLineEdit::Normal);
        dialog.setMinimumWidth(420);
        bool ok = dialog.exec() == QDialog::Accepted;
        QString answer = dialog.textValue();
        dialog.setTextValue(QString());
        if (!ok) return std::nullopt;
        return answer;
    }

    // ---- state -> views -----------------------------------------------------------

    std::optional<double> axis_value(QLineEdit *field, const QString &name, QString *error) {
        QString text = field->text().trimmed();
        if (text.isEmpty() || text.compare("x", Qt::CaseInsensitive) == 0) return std::nullopt;
        double value;
        if (!parse_value(text, value)) {
            if (error && error->isEmpty()) *error = name + " must be a number (or blank to keep it)";
            return std::nullopt;
        }
        return value;
    }

    Target parse_target(QString *error) {
        return Target{axis_value(target_pan_, "Pan", error), axis_value(target_tilt_, "Tilt", error),
                      axis_value(target_zoom_, "Zoom", error)};
    }

    void update_target_markers() {
        Target t = parse_target(nullptr);
        plane_->state.target_pan = t.pan;
        plane_->state.target_tilt = t.tilt;
        meter_pan_->state.target = t.pan;
        meter_tilt_->state.target = t.tilt;
        meter_zoom_->state.target = t.zoom;
        for (QWidget *w : std::initializer_list<QWidget *>{plane_, meter_pan_, meter_tilt_, meter_zoom_}) w->update();
    }

    void update_controls() {
        bool connected = state_ == State::Connected, idle = state_ == State::Idle;
        for (QLineEdit *f : fields_) f->setEnabled(idle);
        for (QWidget *w : std::initializer_list<QWidget *>{ssh_login_, cam_login_, save_ssh_, save_cam_}) w->setEnabled(idle);
        for (QPushButton *b : {route_local_, route_ssh_, vendor_onvif_, vendor_axis_}) b->setEnabled(idle);
        route_local_->setChecked(route_ == 0);
        route_ssh_->setChecked(route_ == 1);
        vendor_onvif_->setChecked(vendor_ == 0);
        vendor_axis_->setChecked(vendor_ == 1);
        for (QWidget *w : ssh_widgets_) w->setVisible(route_ == 1);
        vendor_note_->setText(vendor_ == 1 ? "Native VAPIX with HTTP Digest. Pan/tilt in degrees, integer zoom."
                                           : "ONVIF absolute coordinates with camera ONVIF credentials.");
        connect_->setText(idle ? "Connect"
                          : state_ == State::Connecting ? "Cancel"
                          : connected ? "Disconnect"
                                      : "Disconnecting…");
        connect_->setEnabled(state_ != State::Disconnecting);
        for (QWidget *w : std::initializer_list<QWidget *>{target_pan_, target_tilt_, target_zoom_, move_, stop_, web_})
            w->setEnabled(connected);
        start_->setEnabled(connected && starting_ && !starting_->empty());

        const CameraInfo empty_info;
        const CameraInfo &info = info_ ? *info_ : empty_info;
        plane_->state.enabled = connected;
        refresh_picture_->setEnabled(connected && !taking_picture_);
        plane_->state.pan = info.pan;
        plane_->state.tilt = info.tilt;
        plane_->state.current = connected ? current_ : std::nullopt;
        plane_->state.starting = connected ? starting_ : std::nullopt;
        struct M { MeterView *w; AxisRange r; std::optional<Reading> Snapshot::*axis; };
        for (auto [w, range, axis] : {M{meter_pan_, info.pan, &Snapshot::pan}, M{meter_tilt_, info.tilt, &Snapshot::tilt},
                                      M{meter_zoom_, info.zoom, &Snapshot::zoom}}) {
            MeterState &m = w->state;
            m.enabled = connected;
            m.range = range;
            m.value = connected && current_ && (*current_.*axis) ? std::optional<double>((*current_.*axis)->value) : std::nullopt;
            m.start = connected && starting_ && (*starting_.*axis) ? std::optional<double>((*starting_.*axis)->value) : std::nullopt;
            w->refresh_cursor();
        }
        meter_zoom_->state.integer = info.zoom_integer;
        update_target_markers();

        // Header.
        const Settings &s = settings_;
        title_->setText(connected && info_ ? s.camera_host + " · " + info_->name
                        : state_ == State::Connecting ? "Connecting to " + s.camera_host + "…"
                                                      : "No camera link");
        units_->setText(connected && info_ ? info_->units
                                           : "Choose a route and vendor, enter the camera details, then Connect.");
        const Theme &t = g_theme;
        switch (state_) {
            case State::Idle: chip_->set("OFFLINE", t.secondary); break;
            case State::Connecting: chip_->set("LINKING", t.dark && phosphor_ != Phosphor::System ? t.accent : t.start); break;
            case State::Disconnecting: chip_->set("CLOSING", t.start); break;
            case State::Connected: {
                bool ok = current_ && !current_->empty();
                chip_->set(link_busy(link_) ? "REQUEST" : paused_ ? "READS PAUSED" : ok ? "POSITION OK" : "NO POSITION",
                           paused_ || !ok ? t.start : t.good);
                break;
            }
        }

        // Readouts.
        auto range_text = [](const AxisRange &r) {
            return r.available ? format_value(r.min) + " … " + format_value(r.max) : QString("range not advertised");
        };
        auto reading = [&](std::optional<Reading> Snapshot::*axis) -> std::optional<QString> {
            if (!connected || !current_ || !((*current_).*axis)) return std::nullopt;
            return ((*current_).*axis)->text;
        };
        std::optional<QString> motion;
        if (connected && current_ && !current_->move_status.isEmpty()) motion = current_->move_status;
        readouts_->items = {
            {"PAN", reading(&Snapshot::pan), connected ? range_text(info.pan) : ""},
            {"TILT", reading(&Snapshot::tilt), connected ? range_text(info.tilt) : ""},
            {"ZOOM", reading(&Snapshot::zoom), connected ? range_text(info.zoom) + (info.zoom_integer ? " · integer" : "") : ""},
            {"MOTION", motion, connected && current_ && !motion ? "not reported by camera" : ""},
        };
        readouts_->starting = connected && starting_ ? describe(*starting_) : QString("—");
        readouts_->update();

        QString route = s.route ? "SSH " + s.ssh_user + "@" + s.ssh_host + ":" + QString::number(s.ssh_port) : "LOCAL";
        if (state_ == State::Idle) route = route_ ? "SSH " + ssh_user_->text().trimmed() + "@" + ssh_host_->text().trimmed() + ":" +
                                                        ssh_port_->text().trimmed()
                                                  : "LOCAL";
        footer_->setText(connected ? route + "  ·  " + s.camera_host + ":" + QString::number(s.camera_port) + "  ·  " +
                                         (paused_ ? "auto-refresh PAUSED (Ctrl+R resumes)" : "2 s read-only refresh") +
                                         "  ·  " + (link_busy(link_) ? "busy" : "ready")
                                   : route + "  ·  credentials stay in memory only  ·  Enter connects");
    }

    void show_message(const QString &text, bool error) {
        message_->setText(text);
        message_error_ = error;
        message_->setStyleSheet("color: " + css(error ? g_theme.warning : g_theme.secondary));
    }

    void set_route(int route) {
        if (state_ != State::Idle) return;
        route_ = route;
        update_controls();
    }

    void set_vendor(int vendor) {
        if (state_ != State::Idle) return;
        vendor_ = vendor;
        update_controls();
    }

    // ---- saved logins -----------------------------------------------------------------

    enum { kTyped = -1, kManage = -2, kUnlock = -3 };

    void reload_logins() {
        fill_logins(ssh_login_, LoginStore::Ssh);
        fill_logins(cam_login_, LoginStore::Camera);
    }

    void fill_logins(QComboBox *box, LoginStore store) {
        QString previous = box->currentData().toInt() >= 0 ? box->currentText() : QString();
        box->clear();
        box->addItem("Type a login", kTyped);
        VaultState state = vault_state();
        if (state == VaultState::Unavailable) {
            box->addItem("Saved logins unavailable", kTyped);
        } else if (state == VaultState::Locked) {
            box->addItem("Unlock Saved Logins…", kUnlock);
        } else {
            int i = 0;
            for (const SavedLogin &l : vault_logins(store)) box->addItem(l.name, i++);
        }
        box->insertSeparator(box->count());
        box->addItem("Manage Logins…", kManage);
        int found = previous.isEmpty() ? -1 : box->findText(previous);
        box->setCurrentIndex(found > 0 ? found : 0);
    }

    void login_chosen(QComboBox *box, int index) {
        LoginStore store = box == ssh_login_ ? LoginStore::Ssh : LoginStore::Camera;
        int tag = box->itemData(index).toInt();
        if (tag == kManage) {
            box->setCurrentIndex(0);
            show_manage_logins(this, store);
            return;
        }
        if (tag == kUnlock) {
            box->setCurrentIndex(0);
            if (vault_unlock_dialog(this)) box->showPopup();
            return;
        }
        if (tag < 0) return;
        std::vector<SavedLogin> logins = vault_logins(store);
        if (tag >= static_cast<int>(logins.size())) return;
        const SavedLogin &l = logins[tag];
        if (store == LoginStore::Ssh) {
            ssh_user_->setText(l.user);
            ssh_password_->setText(l.password);
        } else {
            cam_user_->setText(l.user);
            cam_password_->setText(l.password);
        }
    }

    void choose_saved_login(LoginStore store, int index) {  // test hook
        QComboBox *box = store == LoginStore::Ssh ? ssh_login_ : cam_login_;
        int item = box->findData(index);
        if (item < 0) return;
        box->setCurrentIndex(item);
        login_chosen(box, item);
    }

    // The name of the saved login a section's fields still hold, or `fallback`.
    QString chosen_login_name(QComboBox *box, const QString &fallback) {
        return box->currentData().toInt() >= 0 ? box->currentText() : fallback;
    }

    void save_ticked_logins(std::vector<std::pair<SavedLogin, LoginStore>> saved) {
        save_cam_->setChecked(false);
        save_ssh_->setChecked(false);
        if (saved.empty()) return;
        if (!vault_ensure_writable(this)) {
            log_line("Logins not saved.");
        } else {
            for (auto &[login, store] : saved) {
                QString error, kind = store == LoginStore::Ssh ? "SSH" : "camera";
                QString name = qEnvironmentVariable("PTZ_SNAPSHOT_LOGIN_NAME");  // test hook: the name a user would type
                if (name.isEmpty()) name = vault_ask_name(this, store, login.user, login.name);
                if (name.isEmpty()) continue;
                login.name = name;
                if (vault_save(login, store, -1, &error)) log_line("Saved " + kind + " login " + login.name + ".");
                else log_line("Couldn't save the " + kind + " login: " + error, true);
            }
        }
        for (auto &[login, store] : saved) login.password.fill(QChar(0));
    }

    // ---- connection -------------------------------------------------------------------

    std::optional<Settings> read_settings(QString &error) {
        auto port = [&](QLineEdit *field, const QString &name, int &out) {
            bool ok = false;
            int v = field->text().trimmed().toInt(&ok);
            if (ok && v >= 1 && v <= 65535) {
                out = v;
                return true;
            }
            error = name + " must be an integer between 1 and 65535";
            return false;
        };
        Settings s = settings_;
        s.route = route_;
        s.vendor = vendor_;
        s.camera_host = cam_host_->text().trimmed();
        s.camera_user = cam_user_->text().trimmed();
        if (s.camera_host.isEmpty()) { error = "Camera address is required"; return std::nullopt; }
        if (s.camera_user.isEmpty()) { error = "Camera username is required"; return std::nullopt; }
        if (!port(cam_port_, "Camera HTTP port", s.camera_port)) return std::nullopt;
        if (!port(https_port_, "HTTPS port", s.https_port)) return std::nullopt;
        bool ok = false;
        s.profile = profile_->text().trimmed().toInt(&ok);
        if (!ok || s.profile < 0) { error = "Profile index must be a nonnegative integer"; return std::nullopt; }
        if (!parse_value(timeout_->text().trimmed(), s.timeout) || s.timeout <= 0 || s.timeout > 300) {
            error = "Timeout must be a number of seconds between 0 and 300";
            return std::nullopt;
        }
        s.ssh_host = ssh_host_->text().trimmed();
        s.ssh_user = ssh_user_->text().trimmed();
        s.ssh_identity = ssh_key_->text().trimmed();
        if (s.ssh_identity.startsWith("~/")) s.ssh_identity = QDir::homePath() + s.ssh_identity.mid(1);
        if (s.route == 1) {
            if (s.ssh_host.isEmpty()) { error = "SSH host is required"; return std::nullopt; }
            if (s.ssh_user.isEmpty()) { error = "SSH username is required"; return std::nullopt; }
            if (!port(ssh_port_, "SSH port", s.ssh_port)) return std::nullopt;
        }
        return s;
    }

    void connect_camera() {
        // Capture secrets first: nothing below may clear the fields before this.
        QString camera_password = cam_password_->text(), ssh_password = ssh_password_->text();
        QString error;
        auto settings = read_settings(error);
        if (!settings) {
            show_message(error, true);
            return;
        }
        if (camera_password.isEmpty()) {
            show_message("Camera password is required", true);
            cam_password_->setFocus();
            return;
        }
        settings_ = *settings;
        settings_.save();
        state_ = State::Connecting;
        const Settings &s = settings_;
        show_message(s.route ? "Opening SSH tunnel…" : "Connecting…", false);
        log_line("Connecting: " + (s.route ? "SSH via " + s.ssh_user + "@" + s.ssh_host : QString("local")) + " → " +
                 s.camera_host + ":" + QString::number(s.camera_port) + " · " + (s.vendor ? "Axis VAPIX" : "ONVIF"));
        update_controls();
        // What Save login keeps once the connection has proved it.
        std::vector<std::pair<SavedLogin, LoginStore>> saved;
        if (save_cam_->isChecked())
            saved.push_back({{chosen_login_name(cam_login_, s.camera_user), s.camera_user, camera_password}, LoginStore::Camera});
        if (s.route == 1 && save_ssh_->isChecked())
            saved.push_back({{chosen_login_name(ssh_login_, s.ssh_user), s.ssh_user, ssh_password}, LoginStore::Ssh});
        link_connect(link_, s, camera_password, s.route ? ssh_password : QString(), [this, saved](LinkResult result, CameraInfo info) {
            if (result.ok) {
                info_ = info;
                state_ = State::Connected;
                starting_.reset();
                current_.reset();
                failures_ = 0;
                paused_ = false;
                plane_->reset_trail();
                plane_->state.view_moves = info.view_moves;
                plane_->state.picture = QImage();
                plane_->state.picture_note.clear();
                plane_->state.picture_target.reset();
                picture_due_ = 0;
                take_picture();
                show_message("Connected. No movement has been sent.", false);
                log_line("Connected: " + info.name);
                log_line(info.units);
                for (auto [name, range] : {std::pair{"Pan", info.pan}, std::pair{"Tilt", info.tilt}, std::pair{"Zoom", info.zoom}})
                    log_line(QString("  ") + name + " range: " +
                             (range.available ? format_value(range.min) + " … " + format_value(range.max) : "not advertised"));
                refresh();
                save_ticked_logins(saved);
            } else {
                state_ = State::Idle;
                show_message(result.error, true);
                log_line("Connection failed: " + result.error, true);
            }
            update_controls();
        });
        camera_password.fill(QChar(0));
        ssh_password.fill(QChar(0));
    }

    void disconnect(std::function<void()> then = nullptr) {
        if (state_ != State::Connected && state_ != State::Connecting) {
            if (then) then();
            return;
        }
        if (state_ == State::Connecting) link_cancel_connect(link_);
        state_ = State::Disconnecting;
        update_controls();
        log_line("Disconnecting; waiting for any in-flight request (no PTZ stop is sent)");
        link_disconnect(link_, [this, then] {
            state_ = State::Idle;
            info_.reset();
            current_.reset();
            starting_.reset();
            plane_->state.picture = QImage();
            plane_->state.picture_target.reset();
            picture_due_ = 0;
            show_message("Disconnected.", false);
            log_line("Disconnected");
            update_controls();
            if (then) then();
        });
    }

    void toggle_connection() {
        switch (state_) {
            case State::Idle: connect_camera(); break;
            case State::Connecting:
                link_cancel_connect(link_);
                log_line("Cancelling connection…");
                break;
            case State::Connected: disconnect(); break;
            case State::Disconnecting: break;
        }
    }

    void link_lost(const QString &message) {
        log_line(message, true);
        show_message(message, true);
        disconnect();
    }

    // ---- position, moves --------------------------------------------------------------

    void refresh() {
        link_position(link_, [this](LinkResult result, Snapshot snapshot) {
            if (state_ != State::Connected) return;
            next_refresh_ = QDateTime::currentMSecsSinceEpoch() + 2000;
            if (result.ok) {
                failures_ = 0;
                last_read_ = QDateTime::currentMSecsSinceEpoch();
                if (!starting_) {
                    starting_ = snapshot;
                    log_line("Starting coordinates: " + describe(snapshot));
                }
                if (current_ && !(snapshot == *current_)) log_line("Current coordinates: " + describe(snapshot));
                current_ = snapshot;
                plane_->state.current = snapshot;
                plane_->record();
            } else {
                if (result.link_lost) {
                    link_lost(result.error);
                    return;
                }
                ++failures_;
                log_line("Read failed: " + result.error, true);
                if (!starting_) starting_ = Snapshot{};
                if (failures_ >= 3) {
                    paused_ = true;
                    log_line("Auto-refresh paused after 3 failures. Press Ctrl+R to retry.", true);
                }
            }
            update_controls();
        });
        update_controls();
    }

    void tick() {
        if (state_ != State::Connected || link_busy(link_)) return;
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (picture_due_) {
            // A fresh picture once the camera has stopped: a reading taken after the move that says it isn't moving,
            // or, when the camera doesn't say, three seconds.
            qint64 age = now - picture_due_;
            bool reported = current_ && !current_->move_status.isEmpty();
            bool stopped = last_read_ > picture_due_ + 800 && reported && current_->move_status != "MOVING";
            if (stopped || (age >= 3000 && (!reported || paused_)) || age >= 30000) {
                picture_due_ = 0;
                take_picture();
                return;
            }
        }
        if (!paused_ && now >= next_refresh_) refresh();
    }

    void refresh_now() {
        if (state_ != State::Connected) return;
        if (paused_ || failures_) log_line("Auto-refresh resumed");
        failures_ = 0;
        paused_ = false;
        next_refresh_ = 0;
        if (!link_busy(link_)) refresh();
        update_controls();
    }

    void toggle_pause() {
        if (state_ != State::Connected) return;
        paused_ = !paused_;
        log_line(paused_ ? "Auto-refresh paused" : "Auto-refresh resumed");
        update_controls();
    }

    void send(const Target &t, const QString &what) {
        if (link_busy(link_)) log_line("Request in progress; move queued behind it");
        QString summary;
        for (auto [name, v] : {std::pair{"pan", t.pan}, std::pair{"tilt", t.tilt}, std::pair{"zoom", t.zoom}})
            if (v) summary += (summary.isEmpty() ? "" : ", ") + QString(name) + "=" + format_value(*v);
        link_move(link_, t.pan, t.tilt, t.zoom, [this, what, summary](LinkResult result) {
            if (result.ok) {
                log_line(what + " accepted: " + summary + " (movement may still be in progress)");
                next_refresh_ = 0;
                camera_moved();
            } else if (result.link_lost) {
                link_lost(result.error);
                return;
            } else {
                log_line(what + " failed: " + result.error, true);
            }
            update_controls();
        });
        update_controls();
    }

    void send_move() {
        if (state_ != State::Connected) return;
        if (plane_->state.mode == PlaneMode::Picture && plane_->state.picture_target) {
            send_picture_move(*plane_->state.picture_target);
            return;
        }
        QString error;
        Target t = parse_target(&error);
        if (!error.isEmpty()) {
            log_line(error, true);
            QApplication::beep();
            return;
        }
        if (t.empty()) {
            log_line("All axes unchanged; no move sent.");
            return;
        }
        send(t, "Move");
    }

    void return_to_start() {
        if (state_ != State::Connected) return;
        if (!starting_ || starting_->empty()) {
            log_line("Starting coordinates unavailable; no return move sent", true);
            return;
        }
        const Snapshot s = *starting_;
        QStringList missing;
        if (!s.pan) missing << "pan";
        if (!s.tilt) missing << "tilt";
        if (!s.zoom) missing << "zoom";
        log_line("Returning to starting coordinates" +
                 (missing.isEmpty() ? QString() : "; unavailable axes left unchanged: " + missing.join(", ")));
        Target t;
        if (s.pan) t.pan = s.pan->value;
        if (s.tilt) t.tilt = s.tilt->value;
        if (s.zoom) t.zoom = s.zoom->value;
        // Show the start position as the target, so the plane and meter handles move there too.
        target_pan_->setText(s.pan ? s.pan->text : QString());
        target_tilt_->setText(s.tilt ? s.tilt->text : QString());
        target_zoom_->setText(s.zoom ? s.zoom->text : QString());
        update_target_markers();
        send(t, "Return");
    }

    void stop_motion() {
        if (state_ != State::Connected) return;
        link_stop(link_, [this](LinkResult result) {
            if (result.ok) {
                log_line("Stop accepted");
                camera_moved();
            } else if (result.link_lost) { link_lost(result.error); return; }
            else log_line("Stop failed: " + result.error, true);
            next_refresh_ = 0;
        });
    }

    // ---- picture (spec I9-I11) -------------------------------------------------------

    static constexpr const char *kRangeCaption = "POSITION PLANE // XY — CLICK TO AIM, DOUBLE-CLICK TO MOVE";

    void set_plane_mode(PlaneMode mode) {
        bool changed = mode != plane_->state.mode;
        plane_->state.mode = mode;
        show_range_->setChecked(mode == PlaneMode::Range);
        show_picture_->setChecked(mode == PlaneMode::Picture);
        refresh_picture_->setVisible(mode == PlaneMode::Picture);
        plane_panel_->set_caption(mode == PlaneMode::Picture ? "CAMERA PICTURE // VIEW — CLICK TO AIM, DOUBLE-CLICK TO MOVE THERE"
                                                             : kRangeCaption);
        plane_panel_->set_accessory(refresh_picture_->parentWidget());  // re-place it at its new width
        plane_->update();
        QString saved = mode == PlaneMode::Picture ? "picture" : "range";
        if (settings_.plane_mode != saved) {
            settings_.plane_mode = saved;
            settings_.save();
        }
        if (mode == PlaneMode::Picture) {
            fit_window_to_picture();
            if (plane_->state.picture.isNull() || changed) take_picture();
        } else {
            restore_range_height();
        }
        update_controls();
    }

    void refresh_picture() {
        if (state_ != State::Connected) return;
        if (plane_->state.mode != PlaneMode::Picture) set_plane_mode(PlaneMode::Picture);
        else take_picture();
    }

    void take_picture() {
        if (state_ != State::Connected || plane_->state.mode != PlaneMode::Picture || taking_picture_) return;
        taking_picture_ = true;
        link_picture(link_, [this](LinkResult result, QImage image) {
            taking_picture_ = false;
            if (state_ != State::Connected) return;
            PlaneState &s = plane_->state;
            if (result.ok) {
                bool reshaped = s.picture.size() != image.size();
                if (s.picture_stale) s.picture_target.reset();  // the camera has moved since
                s.picture = image;
                s.picture_taken = QDateTime::currentDateTime();
                s.picture_stale = false;
                s.picture_note.clear();
                if (reshaped) {
                    log_line(QString("Camera picture: %1 × %2").arg(image.width()).arg(image.height()) +
                             (info_ && info_->view_moves ? "" : "; this camera can't aim from its picture"));
                    fit_window_to_picture();
                }
            } else if (result.link_lost) {
                link_lost(result.error);
                return;
            } else {
                if (s.picture_note != result.error) log_line("Picture failed: " + result.error, true);
                s.picture_note = result.error;
                s.picture_stale = false;
            }
            plane_->update();
            update_controls();
        });
        update_controls();
    }

    void aim_in_picture(QPointF point) {
        plane_->state.picture_target = QPointF(std::clamp(point.x(), -1.0, 1.0), std::clamp(point.y(), -1.0, 1.0));
        plane_->update();
    }

    void send_picture_move(QPointF point) {
        if (!info_ || !info_->view_moves) {
            log_line("This camera can't aim from its picture; use the target fields or the Range plane.", true);
            return;
        }
        QString describe = QString::asprintf("%.2f %s, %.2f %s of the picture", std::abs(point.x()), point.x() < 0 ? "left" : "right",
                                             std::abs(point.y()), point.y() < 0 ? "down" : "up");
        link_move_in_view(link_, point.x(), point.y(), [this, describe](LinkResult result) {
            if (result.ok) {
                log_line("Move toward the picture point accepted: " + describe + " (movement may still be in progress)");
                next_refresh_ = 0;
                camera_moved();
            } else if (result.link_lost) {
                link_lost(result.error);
                return;
            } else {
                log_line("Move toward the picture point failed: " + result.error, true);
            }
            update_controls();
        });
        update_controls();
    }

    // The picture is out of date once the camera moves; a new one is taken when it stops.
    void camera_moved() {
        if (plane_->state.mode != PlaneMode::Picture || plane_->state.picture.isNull()) return;
        plane_->state.picture_stale = true;
        picture_due_ = QDateTime::currentMSecsSinceEpoch();
        plane_->update();
    }

    // In Picture mode the plane takes the picture's shape: the window grows or shrinks in height, keeping its top
    // edge, within the screen.
    void fit_window_to_picture() {
        if (plane_->state.mode != PlaneMode::Picture || !isVisible() || isMaximized() || isFullScreen()) return;
        layout()->activate();
        const QImage &picture = plane_->state.picture;
        double pw = picture.isNull() ? 16 : picture.width(), ph = picture.isNull() ? 9 : picture.height();
        if (pw <= 0 || plane_->width() <= 0) return;
        int wanted = static_cast<int>(std::round((plane_->width() - 2) * ph / pw + 2));
        int delta = wanted - plane_->height();
        if (std::abs(delta) < 2) return;
        if (!range_height_) range_height_ = height();
        QRect frame = frameGeometry(), inner = geometry();
        int height = inner.height() + delta, top = frame.top();
        if (QScreen *screen = this->screen()) {
            QRect area = screen->availableGeometry();
            int chrome = frame.height() - inner.height();
            height = std::min(height, area.height() - chrome);
            if (top + chrome + height > area.bottom() + 1) top = area.bottom() + 1 - chrome - height;
            top = std::max(top, area.top());
        }
        resize(width(), height);
        if (top != frame.top()) move(frame.left(), top);
    }

    void restore_range_height() {
        if (!range_height_) return;
        int h = range_height_;
        range_height_ = 0;
        if (isMaximized() || isFullScreen()) return;
        resize(width(), h);
    }

    void open_web() {
        if (state_ != State::Connected || web_pending_) return;
        web_pending_ = true;
        log_line("Locating camera web interface (HTTP first, HTTPS fallback; no credentials sent)…");
        link_web_url(link_, [this](LinkResult result, QString url, QString notice) {
            web_pending_ = false;
            if (result.ok) {
                log_line("Camera web interface: " + url + (notice.isEmpty() ? "" : " — " + notice));
                if (!QDesktopServices::openUrl(QUrl(url)))
                    QMessageBox::information(this, "Open the camera web interface",
                                             "The browser could not be opened automatically. Open this address:\n" + url);
            } else if (result.link_lost) {
                link_lost(result.error);
            } else {
                log_line("Camera web interface: " + result.error, true);
            }
        });
    }

    void show_help() {
        QMessageBox::information(
            this, "Coordinate help",
            "Targets are absolute positions, not increments. Units depend on the vendor:\n"
            "• ONVIF: the camera's advertised absolute space (commonly pan/tilt −1…1, zoom 0…1; not degrees).\n"
            "• Axis: pan/tilt in degrees; zoom is an integer in the native range.\n\n"
            "Leave a target field blank (or type x) to keep that axis unchanged.\n"
            "Click the position plane to aim, double-click to aim and move.\n"
            "Picture (Ctrl+2) shows a still picture of the camera's view instead: click a point to aim at it, double-click "
            "(or Move) to turn the camera so that point comes to the centre. A new picture comes once the camera stops, or "
            "with Refresh Picture (Ctrl+Shift+R). Range (Ctrl+1) brings back the whole-range plane.\n"
            "Drag along a travel meter to aim that axis alone, double-click it to aim and move.\n\n"
            "Starting coordinates are the first reading after connecting and stay fixed. "
            "Return to Start (Ctrl+B) sends an absolute move there.\n"
            "Current coordinates refresh every 2 seconds (read-only) unless paused (Ctrl+P).\n"
            "Stop (Ctrl+.) sends a PTZ stop. Disconnecting never sends a stop.\n"
            "Web Interface (Ctrl+L) opens the camera's own page in your browser: HTTP first, HTTPS fallback. "
            "No credentials are sent.\n\n"
            "Shortcuts: Ctrl+K connect, Ctrl+Shift+K disconnect, Ctrl+Enter move, Ctrl+R refresh, Ctrl+Shift+L saved logins, "
            "Ctrl+Alt+1…5 phosphor, Ctrl+Q quit.");
    }

public:
    // ---- Main Menu (spec §13 H1) -------------------------------------------------------

    // The ☰ button: every menu of the app, each as a submenu, in the Mac's order.
    QToolButton *main_menu_button() {
        auto *button = new QToolButton;
        button->setObjectName("menu");
        button->setText("☰");
        button->setFixedSize(34, 28);
        button->setPopupMode(QToolButton::InstantPopup);
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);  // the Edit commands act on the field that keeps the focus
        button->setToolTip("Main Menu: every command of PTZ Control, with its shortcut");
        button->setAccessibleName("Main Menu");
        auto *menu = new QMenu(button);
        button->setMenu(menu);
        connect(menu, &QMenu::aboutToShow, button, [this, menu, button] { build_menu(menu, button->window()); });
        return button;
    }

    // The app's shortcuts, in any window that has a Main Menu.
    void install_shortcuts(QWidget *window) {
        auto key = [window](const QKeySequence &seq, std::function<void()> fn) {
            auto *s = new QShortcut(seq, window);
            s->setContext(Qt::WindowShortcut);
            QObject::connect(s, &QShortcut::activated, window, std::move(fn));
        };
        key(QKeySequence("Ctrl+K"), [this] { if (state_ == State::Idle || state_ == State::Connecting) toggle_connection(); });
        key(QKeySequence("Ctrl+Shift+K"), [this] { disconnect(); });
        key(QKeySequence("Ctrl+Shift+L"), [this] { show_manage_logins(this); });
        key(QKeySequence("Ctrl+Return"), [this] { send_move(); });
        key(QKeySequence("Ctrl+Enter"), [this] { send_move(); });
        key(QKeySequence("Ctrl+B"), [this] { return_to_start(); });
        key(QKeySequence("Ctrl+."), [this] { stop_motion(); });
        key(QKeySequence("Ctrl+R"), [this] { refresh_now(); });
        key(QKeySequence("Ctrl+P"), [this] { toggle_pause(); });
        key(QKeySequence("Ctrl+Shift+R"), [this] { refresh_picture(); });
        key(QKeySequence("Ctrl+1"), [this] { set_plane_mode(PlaneMode::Range); });
        key(QKeySequence("Ctrl+2"), [this] { set_plane_mode(PlaneMode::Picture); });
        key(QKeySequence("Ctrl+L"), [this] { open_web(); });
        key(QKeySequence("Ctrl+Q"), [this] { close(); });
        key(QKeySequence("Ctrl+M"), [window] { window->showMinimized(); });
        key(QKeySequence("Ctrl+W"), [window] { window->close(); });
        key(QKeySequence(Qt::Key_F1), [this] { show_help(); });
        for (int i = 0; i < 5; ++i) key(QKeySequence(QString("Ctrl+Alt+%1").arg(i + 1)), [this, i] { set_theme(i); });
    }

    void build_menu(QMenu *menu, QWidget *window) {
        menu->clear();
        bool connected = state_ == State::Connected;
        auto item = [](QMenu *m, const QString &text, const QString &keys, bool enabled, std::function<void()> fn) {
            QAction *a = m->addAction(keys.isEmpty() ? text : text + "\t" + keys);
            a->setEnabled(enabled);
            QObject::connect(a, &QAction::triggered, m, std::move(fn));
            return a;
        };
        QMenu *app = menu->addMenu("PTZ Control");
        item(app, "About PTZ Control", "", true, [this] { show_about(); });
        app->addSeparator();
        item(app, "Quit PTZ Control", "Ctrl+Q", true, [this] { close(); });

        // Edit acts on the field that has the focus, as its own shortcuts do.
        QPointer<QWidget> focus = window->focusWidget();
        auto *line = qobject_cast<QLineEdit *>(focus.data());
        auto *plain = qobject_cast<QPlainTextEdit *>(focus.data());
        bool editable = (line && !line->isReadOnly()) || (plain && !plain->isReadOnly());
        bool text = line || plain;
        bool selected = (line && line->hasSelectedText()) || (plain && plain->textCursor().hasSelection());
        auto send = [focus](const char *slot) { return [focus, slot] { if (focus) QMetaObject::invokeMethod(focus, slot); }; };
        QMenu *edit = menu->addMenu("Edit");
        item(edit, "Undo", "Ctrl+Z", line ? line->isUndoAvailable() : false, send("undo"));
        item(edit, "Redo", "Ctrl+Shift+Z", line ? line->isRedoAvailable() : false, send("redo"));
        edit->addSeparator();
        item(edit, "Cut", "Ctrl+X", editable && selected, send("cut"));
        item(edit, "Copy", "Ctrl+C", selected, send("copy"));
        item(edit, "Paste", "Ctrl+V", editable, send("paste"));
        item(edit, "Select All", "Ctrl+A", text, send("selectAll"));

        QMenu *camera = menu->addMenu("Camera");
        item(camera, state_ == State::Connecting ? "Cancel Connection" : "Connect", "Ctrl+K",
             state_ == State::Idle || state_ == State::Connecting, [this] { toggle_connection(); });
        item(camera, "Disconnect", "Ctrl+Shift+K", connected, [this] { disconnect(); });
        camera->addSeparator();
        item(camera, "Move to Target", "Ctrl+Enter", connected, [this] { send_move(); });
        item(camera, "Return to Starting Coordinates", "Ctrl+B", connected, [this] { return_to_start(); });
        item(camera, "Stop Motion", "Ctrl+.", connected, [this] { stop_motion(); });
        camera->addSeparator();
        item(camera, "Refresh Position", "Ctrl+R", connected, [this] { refresh_now(); });
        item(camera, paused_ ? "Resume Auto-Refresh" : "Pause Auto-Refresh", "Ctrl+P", connected, [this] { toggle_pause(); });
        item(camera, "Refresh Picture", "Ctrl+Shift+R", connected, [this] { refresh_picture(); });
        camera->addSeparator();
        item(camera, "Open Camera Web Interface", "Ctrl+L", connected, [this] { open_web(); });
        camera->addSeparator();
        item(camera, "Manage Logins…", "Ctrl+Shift+L", vault_state() != VaultState::Unavailable,
             [this] { show_manage_logins(this); });

        QMenu *view = menu->addMenu("View");
        for (auto [title, keys, mode] : {std::tuple{"Whole Range", "Ctrl+1", PlaneMode::Range},
                                         std::tuple{"Camera Picture", "Ctrl+2", PlaneMode::Picture}}) {
            PlaneMode m = mode;
            QAction *a = item(view, title, keys, true, [this, m] { set_plane_mode(m); });
            a->setCheckable(true);
            a->setChecked(plane_->state.mode == m);
        }
        view->addSeparator();
        QMenu *phosphor = view->addMenu("Phosphor");
        for (int i = 0; i < 5; ++i) {
            QAction *a = item(phosphor, phosphor_title(static_cast<Phosphor>(i)), QString("Ctrl+Alt+%1").arg(i + 1), true,
                              [this, i] { set_theme(i); });
            a->setCheckable(true);
            a->setChecked(static_cast<int>(phosphor_) == i);
        }

        QPointer<QWidget> target = window;
        QMenu *windows = menu->addMenu("Window");
        item(windows, "Minimize", "Ctrl+M", true, [target] { if (target) target->showMinimized(); });
        item(windows, target->isMaximized() ? "Restore" : "Maximize", "", true,
             [target] { if (target) target->isMaximized() ? target->showNormal() : target->showMaximized(); });
        item(windows, "Close", "Ctrl+W", true, [target] { if (target) target->close(); });

        QMenu *help = menu->addMenu("Help");
        item(help, "Coordinate Help", "F1", true, [this] { show_help(); });
    }

    void show_about() {
        // QMessageBox shows rich text and opens its links in the browser.
        QMessageBox::about(this, "About PTZ Control",
                           "<p><b>PTZ Control " + QString::fromUtf8(ptz_core_version()).toHtmlEscaped() +
                               "</b></p><p>PTZ Coordinates Controller<br>ONVIF · Axis VAPIX · Local or SSH</p>"
                               "<p>By dunkelherzmann<br><a href=\"https://github.com/dunkelherzmann/ptz-control\">"
                               "https://github.com/dunkelherzmann/ptz-control</a></p>");
    }

    // Test copy (PTZ_MENU_REPORT): the Main Menu's outline, where its buttons are, and whether a menu command acts.
    void write_menu_report(const QString &path) {
        QStringList lines;
        QMenu menu;
        build_menu(&menu, this);
        std::function<void(QMenu *, int)> outline = [&](QMenu *m, int depth) {
            for (QAction *a : m->actions()) {
                if (a->isSeparator()) continue;
                lines << QString(depth * 2, ' ') + a->text().section('\t', 0, 0);
                if (a->menu()) outline(a->menu(), depth + 1);
            }
        };
        outline(&menu, 0);
        auto has_button = [](QWidget *w) {
            for (auto *b : w->findChildren<QToolButton *>("menu"))
                if (b->accessibleName() == "Main Menu") return true;
            return false;
        };
        QDialog *logins = open_manage_logins_for_test(this);
        lines.prepend(QString("manage logins main menu button: ") + (logins && has_button(logins) ? "yes" : "no"));
        delete logins;
        lines.prepend(QString("main menu button: ") + (has_button(this) ? "yes" : "no"));
        int before = static_cast<int>(phosphor_);
        for (QAction *a : menu.findChildren<QAction *>())
            if (a->text().startsWith("Green Phosphor")) a->trigger();
        lines.insert(2, QString("menu command acts: theme %1 -> %2")
                            .arg(phosphor_title(static_cast<Phosphor>(before)), phosphor_title(phosphor_)));
        set_theme(before);
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write((lines.join('\n') + '\n').toUtf8());
    }

private:
    // ---- theme ------------------------------------------------------------------------

    void set_theme(int index) {
        phosphor_ = static_cast<Phosphor>(index);
        settings_.theme = index;
        settings_.save();
        apply_theme();
    }

    void apply_theme() {
        g_theme = make_theme(phosphor_);
        instruments_set_theme(g_theme);
        const Theme &t = g_theme;
        QPalette palette;
        palette.setColor(QPalette::Window, t.panel);
        palette.setColor(QPalette::WindowText, t.text);
        palette.setColor(QPalette::Base, t.field);
        palette.setColor(QPalette::AlternateBase, t.panel);
        palette.setColor(QPalette::Text, t.field_text);
        palette.setColor(QPalette::PlaceholderText, blend(t.field, t.field_text, 0.38));
        palette.setColor(QPalette::Button, t.button);
        palette.setColor(QPalette::ButtonText, t.button_text);
        palette.setColor(QPalette::Highlight, t.accent);
        palette.setColor(QPalette::HighlightedText, t.accent_text);
        palette.setColor(QPalette::ToolTipBase, t.panel);
        palette.setColor(QPalette::ToolTipText, t.text);
        palette.setColor(QPalette::Light, blend(t.button, QColor(255, 255, 255), 0.2));
        palette.setColor(QPalette::Mid, t.border);
        palette.setColor(QPalette::Dark, blend(t.panel, t.border, 0.7));
        palette.setColor(QPalette::Disabled, QPalette::Text, blend(t.field, t.field_text, 0.55));
        palette.setColor(QPalette::Disabled, QPalette::WindowText, blend(t.panel, t.text, 0.5));
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, blend(t.panel, t.button_text, 0.45));
        QApplication::setPalette(palette);

        QString mono = mono_font(12).family();
        QString disabled_ink = css(blend(t.panel, t.button_text, 0.45)), disabled_fill = css(blend(t.panel, t.button, 0.6));
        qApp->setStyleSheet(QString(R"(
            QWidget#console { background: %1; }
            QLabel { color: %2; }
            QLabel#caption, QLabel#secondary { color: %3; }
            QLabel#title { color: %2; font-size: 17px; font-weight: bold; }
            QLineEdit { background: %4; color: %5; border: 1px solid %6; border-radius: 3px; padding: 2px 5px;
                        selection-background-color: %7; selection-color: %8; }
            QLineEdit:focus { border-color: %9; }
            QLineEdit:disabled { color: %10; border-color: %11; }
            QPushButton { background: %12; color: %13; border: 1px solid %6; border-radius: 5px; padding: 4px 12px; min-height: 18px; }
            QPushButton:hover { border-color: %9; }
            QPushButton:pressed { background: %14; }
            QPushButton:disabled { color: %15; background: %16; border-color: %11; }
            QPushButton#segment { background: %4; color: %2; border-radius: 3px; }
            QPushButton#segment:checked { background: %7; color: %8; border-color: %7; }
            QPushButton#segment:checked:disabled { background: %17; color: %18; }
            QPushButton#mini { background: %4; color: %2; border: 1px solid %6; border-radius: 3px; padding: 0px 9px;
                               min-height: 0px; font-size: 11px; }
            QPushButton#mini:checked { background: %7; color: %8; border-color: %7; }
            QPushButton#primary { background: %7; color: %8; border-color: %7; font-weight: bold; }
            QPushButton#primary:hover { border-color: %9; }
            QPushButton#primary:disabled { color: %15; background: %16; border-color: %11; }
            QPushButton#stop { color: %19; }
            QPushButton#stop:disabled { color: %15; }
            QToolButton#menu { background: %12; color: %13; border: 1px solid %6; border-radius: 5px; font-size: 15px; }
            QToolButton#menu::menu-indicator { image: none; width: 0; }
            QToolButton#menu:hover { border-color: %9; }
            QPlainTextEdit { background: %20; color: %2; selection-background-color: %7; selection-color: %8;
                             font-family: "%21"; }
            QMenu { background: %20; color: %2; border: 1px solid %6; padding: 4px; }
            QMenu::item { padding: 4px 24px 4px 20px; }
            QMenu::item:selected { background: %7; color: %8; }
            QMenu::item:disabled { color: %15; }
            QMenu::separator { height: 1px; background: %6; margin: 4px 6px; }
            QDialog, QMessageBox, QInputDialog { background: %20; }
            QComboBox { background: %4; color: %5; border: 1px solid %6; border-radius: 3px; padding: 2px 5px; }
            QComboBox:hover, QComboBox:focus { border-color: %9; }
            QComboBox:disabled { color: %10; border-color: %11; }
            QComboBox QAbstractItemView { background: %20; color: %2; selection-background-color: %7;
                                          selection-color: %8; border: 1px solid %6; }
            QCheckBox { color: %2; spacing: 6px; }
            QCheckBox:disabled { color: %15; }
            QCheckBox::indicator { width: 12px; height: 12px; border: 1px solid %9; border-radius: 3px; background: %4; }
            QCheckBox::indicator:checked { background: %7; border-color: %7; }
            QCheckBox::indicator:disabled { border-color: %11; }
            QTableWidget { background: %4; alternate-background-color: %20; color: %5; gridline-color: %6;
                           border: 1px solid %6; selection-background-color: %7; selection-color: %8; }
            QHeaderView::section { background: %20; color: %3; border: none; border-right: 1px solid %6;
                                   border-bottom: 1px solid %6; padding: 4px 6px; }
        )")
                                .arg(css(t.background), css(t.text), css(t.secondary), css(t.field), css(t.field_text),
                                     css(t.border), css(t.accent), css(t.accent_text), css(t.primary))
                                .arg(css(blend(t.field, t.field_text, 0.55)), css(blend(t.panel, t.border, 0.5)),
                                     css(t.button), css(t.button_text), css(blend(t.button, t.text, 0.15)), disabled_ink,
                                     disabled_fill, css(blend(t.panel, t.accent, 0.6)), css(blend(t.panel, t.accent_text, 0.8)))
                                .arg(css(t.warning), css(t.panel), mono));
        show_message(message_->text(), message_error_);

        // Re-render the event buffer in the new colours; warnings keep theirs.
        log_->clear();
        for (const auto &[text, warning] : log_lines_) append_log(text, warning);
        update_controls();
        update();
    }

    // ---- UI test hook (PTZ_SNAPSHOT_PATH) ---------------------------------------------

    // PTZ_SNAPSHOT_SAVE_LOGIN ticks the camera's Save login box; PTZ_SNAPSHOT_USE_LOGIN fills the camera fields
    // from the first saved camera login; PTZ_SNAPSHOT_LOGINS_PATH also saves pictures of Manage Logins and its form.
    void run_snapshot_script() {
        if (QByteArray name = qgetenv("PTZ_THEME"); !name.isEmpty())
            for (int i = 0; i < 5; ++i)
                if (phosphor_title(static_cast<Phosphor>(i)).toLower().startsWith(QString::fromUtf8(name).toLower())) set_theme(i);
        if (QString path = qEnvironmentVariable("PTZ_MENU_REPORT"); !path.isEmpty()) write_menu_report(path);
        if (qEnvironmentVariableIsSet("PTZ_SNAPSHOT_SAVE_LOGIN")) save_cam_->setChecked(true);
        if (qEnvironmentVariableIsSet("PTZ_SNAPSHOT_USE_LOGIN")) choose_saved_login(LoginStore::Camera, 0);
        if (qEnvironmentVariableIsSet("PTZ_SNAPSHOT_PICTURE")) {
            run_picture_script();
            return;
        }
        connect_camera();
        QTimer::singleShot(3000, this, [this] {
            target_pan_->setText("0.55");
            target_tilt_->setText("0.22");
            target_zoom_->setText("0.4");
            send_move();
        });
        QTimer::singleShot(4200, this, [this] {
            target_pan_->setText("-0.3");
            target_tilt_->setText("0.6");
            if (meter_zoom_->state.on_aim) meter_zoom_->state.on_aim(0.75);  // as if dragged along the zoom meter
            if (qEnvironmentVariableIsSet("PTZ_SNAPSHOT_RETURN")) return_to_start();
        });
        QTimer::singleShot(7000, this, [this] {
            grab().save(qEnvironmentVariable("PTZ_SNAPSHOT_PATH"));
            if (QString path = qEnvironmentVariable("PTZ_SNAPSHOT_LOGINS_PATH"); !path.isEmpty()) {
                manage_logins_snapshot(this, LoginStore::Camera).save(path);
                QString form = path;
                form.replace(".png", "-form.png");
                login_form_snapshot(this, LoginStore::Camera).save(form);
            }
            close();
        });
    }

    // PTZ_SNAPSHOT_PICTURE: Picture mode, aim at a point of the picture (a picture of that is saved to
    // PTZ_SNAPSHOT_AIM_PATH), move there, and save the window once the new picture has come.
    void run_picture_script() {
        set_plane_mode(PlaneMode::Picture);
        connect_camera();
        QTimer::singleShot(3000, this, [this] { if (plane_->state.on_picture_aim) plane_->state.on_picture_aim(QPointF(0.45, 0.3)); });
        QTimer::singleShot(3400, this, [this] {
            if (QString path = qEnvironmentVariable("PTZ_SNAPSHOT_AIM_PATH"); !path.isEmpty()) grab().save(path);
            send_move();
        });
        QTimer::singleShot(4400, this, [this] {
            if (QString path = qEnvironmentVariable("PTZ_SNAPSHOT_MOVING_PATH"); !path.isEmpty()) grab().save(path);
        });
        QTimer::singleShot(10000, this, [this] {
            grab().save(qEnvironmentVariable("PTZ_SNAPSHOT_PATH"));
            close();
        });
    }

    // ---- state ------------------------------------------------------------------------

    Settings settings_;
    Phosphor phosphor_ = Phosphor::System;
    CameraLink *link_ = nullptr;
    State state_ = State::Idle;
    std::optional<CameraInfo> info_;
    std::optional<Snapshot> starting_, current_;
    int failures_ = 0;
    bool paused_ = false;
    qint64 next_refresh_ = 0;
    bool web_pending_ = false, closing_ = false, message_error_ = false, taking_picture_ = false;
    qint64 picture_due_ = 0, last_read_ = 0;  // when the camera last moved (a fresh picture is due), the last reading
    int range_height_ = 0;                    // the window's height in Range mode, restored when leaving Picture mode
    int route_ = 0, vendor_ = 0;
    std::deque<std::pair<QString, bool>> log_lines_;

    QComboBox *ssh_login_, *cam_login_;
    QCheckBox *save_ssh_, *save_cam_;
    QPushButton *route_local_, *route_ssh_, *vendor_onvif_, *vendor_axis_, *connect_, *move_, *start_, *stop_, *web_;
    QLineEdit *ssh_host_, *ssh_user_, *ssh_password_, *ssh_port_, *ssh_key_, *cam_host_, *cam_port_, *cam_user_,
        *cam_password_, *https_port_, *profile_, *timeout_, *target_pan_, *target_tilt_, *target_zoom_;
    std::vector<QLineEdit *> fields_;
    std::vector<QWidget *> ssh_widgets_;
    QLabel *vendor_note_, *message_, *title_, *units_, *footer_;
    Chip *chip_;
    QToolButton *menu_button_;
    PlaneView *plane_;
    Panel *plane_panel_;
    QPushButton *show_range_, *show_picture_;
    RefreshButton *refresh_picture_;
    MeterView *meter_pan_, *meter_tilt_, *meter_zoom_;
    Readouts *readouts_;
    QPlainTextEdit *log_;
};

}  // namespace

Console *g_console = nullptr;

QWidget *create_console() { return g_console = new Console; }

void attach_main_menu(QWidget *window, QBoxLayout *row) {
    if (!g_console) return;
    row->addWidget(g_console->main_menu_button(), 0, Qt::AlignTop);
    g_console->install_shortcuts(window);
}
