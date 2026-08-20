#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QQmlApplicationEngine>

#include <memory>

#include "appcontroller.h"
#include "daemoncontroller.h"
#include "singleinstance.h"
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

    // With the applet running, the window is usually hidden rather than
    // closed, so launching the app again means "show me the window I already
    // have", not "start a second copy that fights this one for the tray".
    if (SingleInstance::notifyRunningInstance()) {
        return 0;
    }
    SingleInstance instance;
    // A guard that could not be installed is not worth refusing to start
    // over; the app simply loses the protection.
    (void)instance.listen();

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

    // Without an icon in the tray there is nowhere to restore the window
    // from, so the app behaves like an ordinary one: it always opens a window
    // and quits when that window closes. That is also the default, since the
    // tray is opt-in.
    appController->setTrayAvailable(TrayIcon::isAvailable());
    appController->setStartHidden(appController->trayActive() && parser.isSet(backgroundOption));

    // Owned here rather than parented to the application object so it is
    // destroyed (and the icon withdrawn) on every exit path. Built whenever
    // the session has a tray at all, even if the preference is off: showing
    // and hiding one icon is cheaper and less fragile than creating and
    // destroying the object as the user toggles the setting.
    std::unique_ptr<TrayIcon> trayIcon;
    if (appController->trayAvailable()) {
        trayIcon = std::make_unique<TrayIcon>(daemonController, appController);
    }

    const auto applyTrayState = [&app, appController, &trayIcon]() {
        if (trayIcon) {
            trayIcon->setVisible(appController->trayActive());
        }
        // Closing the last window ends the program unless the applet is meant
        // to carry on without it -- the window hides itself in that case, so
        // Qt would not see a close at all, but a window that fails to hide
        // must not take the applet down with it.
        app.setQuitOnLastWindowClosed(!appController->hideOnClose());
    };
    // The lambdas hold references to locals of this function, so the
    // connections are dropped before any of them go out of scope.
    const QMetaObject::Connection trayStateConnection =
        QObject::connect(appController, &AppController::trayActiveChanged, &app, applyTrayState);
    const QMetaObject::Connection closeBehaviourConnection =
        QObject::connect(appController, &AppController::closeToTrayChanged, &app, applyTrayState);
    applyTrayState();

    // Another launch asking for the window: the same request the tray menu's
    // "Settings…" entry makes.
    const QMetaObject::Connection showRequestConnection = QObject::connect(
        &instance, &SingleInstance::showRequested, appController,
        [appController]() { appController->requestShowWindow(); });

    engine.loadFromModule("Ds4Translator", "Main");
    const auto dropConnections = [&]() {
        QObject::disconnect(trayStateConnection);
        QObject::disconnect(closeBehaviourConnection);
        QObject::disconnect(showRequestConnection);
    };
    if (engine.rootObjects().isEmpty()) {
        dropConnections();
        return 1;
    }

    const int status = app.exec();
    dropConnections();
    return status;
}
