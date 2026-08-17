#ifndef DS4_UI_IPCCLIENT_H
#define DS4_UI_IPCCLIENT_H

#include <QList>
#include <QLocalSocket>
#include <QObject>
#include <QString>
#include <QTimer>

// Asynchronous client for the daemon's control socket
// (/run/ds4-translator.sock, see src/main.cpp).
//
// The daemon serves one command per connection: it accepts, reads a single
// command, writes the response and closes. So every request here opens its
// own socket, and requests are queued so a slow or unreachable daemon can
// never interleave two exchanges on the same connection.
class IpcClient : public QObject {
    Q_OBJECT

public:
    // Deliberately long: set-type/set-name/set-backend recreate the virtual
    // device, which routinely takes seconds. Anything shorter turns a normal
    // slow command into a bogus "daemon unreachable" error.
    static constexpr int kDefaultTimeoutMs = 10000;

    explicit IpcClient(QObject *parent = nullptr);

    // Queues a command. The reply (or the failure) always comes back through
    // exactly one replyReady() with the same command string.
    void send(const QString &command);

    // True while a request is in flight or waiting in the queue.
    bool isBusy() const;

signals:
    // ok == false means the exchange itself failed (daemon unreachable,
    // timeout, closed connection); response then holds the error text.
    void replyReady(const QString &command, bool ok, const QString &response);

private slots:
    void onConnected();
    void onReadyRead();
    void onErrorOccurred(QLocalSocket::LocalSocketError error);
    void onDisconnected();
    void onTimeout();

private:
    void startNext();
    void finish(bool ok, const QString &response);

    QLocalSocket *m_socket;
    QTimer *m_timeout;
    QList<QString> m_queue;
    QString m_current;
    QByteArray m_buffer;
    bool m_inFlight = false;
};

#endif // DS4_UI_IPCCLIENT_H
