#ifndef DS4_UI_APPCONTROLLER_H
#define DS4_UI_APPCONTROLLER_H

#include <QObject>
#include <QQmlEngine>

// Shared state between the tray applet (C++, needs QtWidgets for the menu)
// and the settings window (QML).
//
// The tray cannot touch the window directly -- it is a QML object created by
// the engine -- so requests travel through here as signals the window listens
// for, and the window asks here whether hiding itself is safe.
class AppController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // False when the session has no system tray (no StatusNotifierItem host,
    // e.g. a bare Wayland compositor with no panel). The window must then
    // stay a normal application window: hiding it would leave the process
    // running with no way to get it back.
    Q_PROPERTY(bool trayAvailable READ trayAvailable NOTIFY trayAvailableChanged)
    // True when the applet was started in the background, so the window
    // should not be shown until the user asks for it.
    Q_PROPERTY(bool startHidden READ startHidden CONSTANT)

public:
    explicit AppController(QObject *parent = nullptr);

    bool trayAvailable() const { return m_trayAvailable; }
    void setTrayAvailable(bool available);

    bool startHidden() const { return m_startHidden; }
    void setStartHidden(bool hidden) { m_startHidden = hidden; }

    // Called from the tray menu.
    void requestShowWindow() { emit showWindowRequested(); }

signals:
    void trayAvailableChanged();
    void showWindowRequested();

private:
    bool m_trayAvailable = false;
    bool m_startHidden = false;
};

#endif // DS4_UI_APPCONTROLLER_H
