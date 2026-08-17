#ifndef DS4_UI_APPCONTROLLER_H
#define DS4_UI_APPCONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QString>

// Shared state between the tray applet (C++, needs QtWidgets for the menu)
// and the settings window (QML), plus the app's own persisted preferences.
//
// The tray cannot touch the window directly -- it is a QML object created by
// the engine -- so requests travel through here as signals the window listens
// for, and the window asks here whether hiding itself is safe.
class AppController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // False when the session has no system tray (no StatusNotifierItem host,
    // e.g. a bare Wayland compositor with no panel).
    Q_PROPERTY(bool trayAvailable READ trayAvailable NOTIFY trayAvailableChanged)
    // User preference, persisted. Off by default: the tray icon is opt-in, so
    // a first run behaves like an ordinary application window.
    Q_PROPERTY(bool trayEnabled READ trayEnabled WRITE setTrayEnabled NOTIFY trayEnabledChanged)
    // Whether an icon is actually in the tray right now. The window must only
    // hide itself on close when this is true: otherwise the process would keep
    // running with no way to get it back.
    Q_PROPERTY(bool trayActive READ trayActive NOTIFY trayActiveChanged)
    // Whether the autostart entry exists. Off by default -- nothing is written
    // to the user's session config until they ask for it here.
    Q_PROPERTY(bool autostartEnabled READ autostartEnabled WRITE setAutostartEnabled NOTIFY
                   autostartEnabledChanged)
    // Last failure while writing a preference (empty when all is well).
    Q_PROPERTY(QString settingsError READ settingsError NOTIFY settingsErrorChanged)
    // True when the applet was started in the background, so the window
    // should not be shown until the user asks for it.
    Q_PROPERTY(bool startHidden READ startHidden CONSTANT)

public:
    explicit AppController(QObject *parent = nullptr);

    bool trayAvailable() const { return m_trayAvailable; }
    void setTrayAvailable(bool available);

    bool trayEnabled() const { return m_trayEnabled; }
    void setTrayEnabled(bool enabled);

    bool trayActive() const { return m_trayAvailable && m_trayEnabled; }

    bool autostartEnabled() const { return m_autostartEnabled; }
    void setAutostartEnabled(bool enabled);

    QString settingsError() const { return m_settingsError; }

    bool startHidden() const { return m_startHidden; }
    void setStartHidden(bool hidden) { m_startHidden = hidden; }

    // Called from the tray menu.
    void requestShowWindow() { emit showWindowRequested(); }

signals:
    void trayAvailableChanged();
    void trayEnabledChanged();
    void trayActiveChanged();
    void autostartEnabledChanged();
    void settingsErrorChanged();
    void showWindowRequested();

private:
    // Path of the XDG autostart entry this app owns.
    static QString autostartFilePath();
    // Writes the entry, pointing at the running binary with --background.
    bool writeAutostartFile(QString *error) const;
    void setSettingsError(const QString &error);

    bool m_trayAvailable = false;
    bool m_trayEnabled = false;
    bool m_autostartEnabled = false;
    bool m_startHidden = false;
    QString m_settingsError;
};

#endif // DS4_UI_APPCONTROLLER_H
