#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QQmlApplicationEngine>

#include <memory>

#include "appcontroller.h"
#include "daemoncontroller.h"
#include "trayicon.h"

int main(int argc, char *argv[]) {
    // QApplication, not QGuiApplication: the tray applet needs QtWidgets for
    // QSystemTrayIcon and its menu. The window itself is still QML.
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ds4-translator-ui"));
    app.setApplicationDisplayName(QStringLiteral("DS4 Translator"));
    app.setOrganizationName(QStringLiteral("ds4-translator"));
    // Must match the installed .desktop file's basename, or the desktop
    // portal cannot look this app up ("App info not found for ...").
    app.setDesktopFileName(QStringLiteral("ds4-translator-ui"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Settings window and tray applet for the ds4-translator daemon."));
    parser.addHelpOption();
    QCommandLineOption backgroundOption(
        QStringList{QStringLiteral("b"), QStringLiteral("background")},
        QStringLiteral("Start in the tray without opening the settings window "
                       "(ignored when the session has no tray)."));
    parser.addOption(backgroundOption);
    parser.process(app);

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        []() { QCoreApplication::exit(1); }, Qt::QueuedConnection);

    auto *appController =
        engine.singletonInstance<AppController *>(QStringLiteral("Ds4Translator"),
                                                  QStringLiteral("AppController"));
    auto *daemonController =
        engine.singletonInstance<DaemonController *>(QStringLiteral("Ds4Translator"),
                                                     QStringLiteral("DaemonController"));
    if (!appController || !daemonController) {
        qCritical("Failed to create the application singletons.");
        return 1;
    }

    // Without a tray there is nowhere to restore the window from, so the app
    // behaves like an ordinary one: it always opens a window and quits when
    // that window closes.
    const bool tray = TrayIcon::isAvailable();
    appController->setTrayAvailable(tray);
    appController->setStartHidden(tray && parser.isSet(backgroundOption));
    app.setQuitOnLastWindowClosed(!tray);

    // Owned here rather than parented to the application object so it is
    // destroyed (and the icon withdrawn) on every exit path.
    std::unique_ptr<TrayIcon> trayIcon;
    if (tray) {
        trayIcon = std::make_unique<TrayIcon>(daemonController, appController);
    }

    engine.loadFromModule("Ds4Translator", "Main");
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    return app.exec();
}
