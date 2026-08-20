#ifndef DS4_UI_SINGLEINSTANCE_H
#define DS4_UI_SINGLEINSTANCE_H

#include <QLocalServer>
#include <QObject>
#include <QString>

// Keeps one copy of the application running per user session.
//
// With the tray applet the window is often hidden rather than closed, so
// launching the app again (from a menu, a shortcut, the terminal) would
// otherwise start a second copy that fights the first over the tray icon and
// leaves the user staring at the window they already had. Instead the second
// copy asks the first to show itself and exits.
//
// The handshake is a Unix socket in the session's runtime directory: the
// first instance listens on it, later ones connect and send a single word.
class SingleInstance : public QObject {
    Q_OBJECT

public:
    explicit SingleInstance(QObject *parent = nullptr);

    // Sends a show request to an already running instance. Returns true when
    // one answered, which means this process should exit without starting a
    // window of its own.
    static bool notifyRunningInstance();

    // Claims the socket for this process. Returns false when it could not be
    // claimed (an unusable runtime directory, a race with another launch), in
    // which case the app still runs -- being unable to guard against a second
    // copy is not a reason to refuse to start.
    bool listen();

signals:
    // Another launch asked for the window.
    void showRequested();

private:
    static QString socketPath();

    QLocalServer m_server;
};

#endif // DS4_UI_SINGLEINSTANCE_H
