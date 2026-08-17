#include <QGuiApplication>
#include <QQmlApplicationEngine>

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ds4-translator-ui"));
    app.setApplicationDisplayName(QStringLiteral("DS4 Translator"));
    app.setOrganizationName(QStringLiteral("ds4-translator"));
    app.setDesktopFileName(QStringLiteral("ds4-translator-ui"));

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        []() { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("Ds4Translator", "Main");

    return app.exec();
}
