#include "singleinstance.h"

#include <QDir>
#include <QLocalSocket>
#include <QStandardPaths>

#include <unistd.h>

namespace {

// One socket per user session. The runtime directory is per-user and cleaned
// up at logout, which is exactly the lifetime this guard should have.
const char *const kSocketName = "ds4-translator-ui.socket";

// The running instance answers immediately or not at all; this is a local
// socket to a process that is already up, so there is nothing slow to wait
// for, and a hung peer must not delay the launch.
constexpr int kConnectTimeoutMs = 300;

const char *const kShowMessage = "show\n";

} // namespace

SingleInstance::SingleInstance(QObject *parent) : QObject(parent) {
    connect(&m_server, &QLocalServer::newConnection, this, [this]() {
        while (QLocalSocket *connection = m_server.nextPendingConnection()) {
            // The message itself carries no information beyond "someone tried
            // to launch me again", so it is not read: the connection is the
            // signal.
            connection->deleteLater();
            emit showRequested();
        }
    });
}

QString SingleInstance::socketPath() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (dir.isEmpty()) {
        // No XDG_RUNTIME_DIR (a bare login shell, some containers). TempLocation
        // is world-writable, so the name is qualified with the user id to keep
        // two users' sockets apart.
        dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
              QLatin1String("/ds4-translator-ui-") + QString::number(::geteuid());
        QDir().mkpath(dir);
    }
    return dir + QLatin1Char('/') + QLatin1String(kSocketName);
}

bool SingleInstance::notifyRunningInstance() {
    QLocalSocket socket;
    socket.connectToServer(socketPath());
    if (!socket.waitForConnected(kConnectTimeoutMs)) {
        return false;
    }
    socket.write(kShowMessage);
    socket.waitForBytesWritten(kConnectTimeoutMs);
    socket.disconnectFromServer();
    return true;
}

bool SingleInstance::listen() {
    const QString path = socketPath();
    if (m_server.listen(path)) {
        return true;
    }
    // A socket file left behind by an instance that was killed rather than
    // shut down. Nothing answered it a moment ago (notifyRunningInstance()
    // runs first), so it is stale and safe to take over.
    QLocalServer::removeServer(path);
    return m_server.listen(path);
}
