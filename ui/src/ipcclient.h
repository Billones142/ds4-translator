#ifndef DS4_UI_IPCCLIENT_H
#define DS4_UI_IPCCLIENT_H

#include <QList>
#include <QObject>
#include <QSocketNotifier>
#include <QString>
#include <QTimer>

#include <string>

// Event-loop wrapper around the shared protocol client in src/ipc-client.cpp.
//
// The socket path, the connect/write/read calls and the response framing all
// come from ds4ipc, exactly as ds4-ctl uses them; the only thing added here
// is asynchrony -- the command is written on a blocking fd (a single short
// line), then the fd goes non-blocking and the response is read from a
// QSocketNotifier so the window never freezes while the daemon works.
//
// The daemon serves one command per connection: it accepts, reads a single
// command, writes the response and closes. So every request opens its own
// socket, and requests are queued rather than interleaved.
class IpcClient : public QObject {
    Q_OBJECT

public:
    explicit IpcClient(QObject *parent = nullptr);
    ~IpcClient() override;

    // Queues a command. The reply (or the failure) always comes back through
    // exactly one replyReady() with the same command string.
    void send(const QString &command);

    // True while a request is in flight or waiting in the queue.
    bool isBusy() const;

signals:
    // ok == false means the exchange itself failed (daemon unreachable,
    // timeout, closed with no response); response then holds the error text.
    void replyReady(const QString &command, bool ok, const QString &response);

private slots:
    void onReadable();
    void onTimeout();

private:
    void startNext();
    void finish(bool ok, const QString &response);
    void closeConnection();

    QTimer *m_timeout;
    QSocketNotifier *m_notifier = nullptr;
    QList<QString> m_queue;
    QString m_current;
    std::string m_buffer;
    int m_fd = -1;
    bool m_inFlight = false;
};

#endif // DS4_UI_IPCCLIENT_H
