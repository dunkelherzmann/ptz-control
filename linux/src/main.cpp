// PTZ Control for Linux: entry point.
#include "app.h"

#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include <QIcon>
#include <QStyleFactory>
#include <QWidget>

int main(int argc, char **argv) {
    // ssh runs this executable as SSH_ASKPASS; relay its prompt and exit before any UI.
    if (ptz_is_askpass_invocation()) return ptz_askpass_main(argc, argv);

    // X11 (or XWayland) first: it behaves the same on every desktop. Pure Wayland
    // sessions without XWayland fall back to the native Wayland plugin.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "xcb;wayland");
    // Inside the AppImage only the bundled plugins match the bundled Qt; never load the
    // system's (a different Qt version would crash) or a desktop theme plugin we don't ship.
    if (qEnvironmentVariableIsSet("APPIMAGE")) {
        qunsetenv("QT_PLUGIN_PATH");
        qunsetenv("QT_QPA_PLATFORMTHEME");
        qunsetenv("QT_STYLE_OVERRIDE");
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName("PTZ Control");
    QApplication::setApplicationVersion(QString::fromUtf8(ptz_core_version()));
    QApplication::setOrganizationName("PTZControl");
    QGuiApplication::setDesktopFileName("ptz-control");
    QApplication::setStyle(QStyleFactory::create("Fusion"));

    // Resources travel with the binary: <prefix>/bin/ptz-control, <prefix>/share/...
    QDir share(QApplication::applicationDirPath() + "/../share");
    for (const QString &name : QDir(share.filePath("ptz-control/fonts")).entryList({"*.ttf", "*.otf"}, QDir::Files))
        QFontDatabase::addApplicationFont(share.filePath("ptz-control/fonts/" + name));
    QIcon icon;
    for (int size : {16, 32, 64, 128, 256, 512}) {
        QString path = share.filePath(QString("icons/hicolor/%1x%1/apps/ptz-control.png").arg(size));
        if (QFile::exists(path)) icon.addFile(path, QSize(size, size));
    }
    QApplication::setWindowIcon(icon);

    QWidget *console = create_console();
    console->setAttribute(Qt::WA_DeleteOnClose);
    console->show();
    return app.exec();
}
