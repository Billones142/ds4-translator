#include "ipcclient.h"

// Must stay in sync with the daemon and ds4-ctl (src/main.cpp, src/ctl.cpp).
static const char *kSocketPath = "/run/ds4-translator.sock";

IpcClient::IpcClient(QObject *parent)
    : QObject(parent), m_socket(new QLocalSocket(this)), m_timeout(new QTimer(this)) {
    m_timeout->setSingleShot(true);
    m_timeout->setInterval(kDefaultTimeoutMs);

    connect(m_socket, &QLocalSocket::connected, this, &IpcClient::onConnected);
    connect(m_socket, &QLocalSocket::readyRead, this, &IpcClient::onReadyRead);
    connect(m_socket, &QLocalSocket::errorOccurred, this, &IpcClient::onErrorOccurred);
    connect(m_socket, &QLocalSocket::disconnected, this, &IpcClient::onDisconnected);
    connect(m_timeout, &QTimer::timeout, this, &IpcClient::onTimeout);
}

bool IpcClient::isBusy() const {
    return m_inFlight || !m_queue.isEmpty();
}

void IpcClient::send(const QString &command) {
    m_queue.append(command);
    if (!m_inFlight) {
        startNext();
    }
}

void IpcClient::startNext() {
    if (m_inFlight || m_queue.isEmpty()) {
        return;
    }
    m_current = m_queue.takeFirst();
    m_buffer.clear();
    m_inFlight = true;
    m_socket->abort();
    m_timeout->start();
    m_socket->connectToServer(QString::fromLatin1(kSocketPath));
}

void IpcClient::onConnected() {
    // The daemon reads one command and does not expect a trailing newline,
    // but it strips one if present -- send it bare, exactly like ds4-ctl.
    const QByteArray payload = m_current.toUtf8();
    if (m_socket->write(payload) < 0) {
        finish(false, tr("Failed to write to daemon: %1").arg(m_socket->errorString()));
    }
}

void IpcClient::onReadyRead() {
    m_buffer.append(m_socket->readAll());
}

void IpcClient::onDisconnected() {
    if (!m_inFlight) {
        return;
    }
    // The daemon closes the connection right after the response, so
    // disconnect is the end-of-message marker.
    m_buffer.append(m_socket->readAll());
    if (m_buffer.isEmpty()) {
        finish(false, tr("No response from daemon (connection closed)."));
        return;
    }
    QString response = QString::fromUtf8(m_buffer);
    while (response.endsWith('\n') || response.endsWith('\r')) {
        response.chop(1);
    }
    finish(true, response);
}

void IpcClient::onErrorOccurred(QLocalSocket::LocalSocketError error) {
    if (!m_inFlight) {
        return;
    }
    if (error == QLocalSocket::PeerClosedError) {
        // Normal end of exchange; onDisconnected() handles it.
        return;
    }
    if (error == QLocalSocket::ConnectionRefusedError ||
        error == QLocalSocket::ServerNotFoundError ||
        error == QLocalSocket::SocketAccessError) {
        finish(false, tr("Cannot reach the translation daemon at %1 "
                         "(is ds4-translator.service running?): %2")
                          .arg(QString::fromLatin1(kSocketPath), m_socket->errorString()));
        return;
    }
    finish(false, tr("Socket error: %1").arg(m_socket->errorString()));
}

void IpcClient::onTimeout() {
    if (!m_inFlight) {
        return;
    }
    finish(false, tr("Timed out after %1 ms waiting for the daemon.").arg(kDefaultTimeoutMs));
}

void IpcClient::finish(bool ok, const QString &response) {
    m_timeout->stop();
    m_socket->abort();
    m_inFlight = false;
    const QString command = m_current;
    m_current.clear();
    m_buffer.clear();
    emit replyReady(command, ok, response);
    startNext();
}
