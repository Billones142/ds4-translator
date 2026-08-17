#include "ipcclient.h"

#include <unistd.h>

#include "ipc-client.h"

IpcClient::IpcClient(QObject *parent) : QObject(parent), m_timeout(new QTimer(this)) {
    m_timeout->setSingleShot(true);
    m_timeout->setInterval(ds4ipc::kDefaultTimeoutMs);
    connect(m_timeout, &QTimer::timeout, this, &IpcClient::onTimeout);
}

IpcClient::~IpcClient() {
    closeConnection();
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

    std::string error;
    // Blocking for connect and write: both are immediate on a Unix socket
    // whose server is listening, and the command is one short line.
    m_fd = ds4ipc::connect_socket(&error);
    if (m_fd < 0) {
        finish(false, QString::fromStdString(error));
        return;
    }
    ds4ipc::set_timeout(m_fd, ds4ipc::kDefaultTimeoutMs);

    if (!ds4ipc::write_command(m_fd, m_current.toStdString(), &error)) {
        finish(false, QString::fromStdString(error));
        return;
    }

    // The response is what can take seconds, so only that part is async.
    if (!ds4ipc::set_non_blocking(m_fd, &error)) {
        finish(false, QString::fromStdString(error));
        return;
    }

    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &IpcClient::onReadable);
    m_timeout->start();
}

void IpcClient::onReadable() {
    if (!m_inFlight) {
        return;
    }

    std::string error;
    for (;;) {
        const ds4ipc::ReadStatus status = ds4ipc::read_available(m_fd, &m_buffer, &error);
        if (status == ds4ipc::ReadStatus::Data) {
            continue; // drain what the kernel already has
        }
        if (status == ds4ipc::ReadStatus::Again) {
            return; // wait for the next notification
        }
        if (status == ds4ipc::ReadStatus::Error) {
            finish(false, QString::fromStdString(error));
            return;
        }
        break; // Eof: the daemon closed, so the response is complete
    }

    if (m_buffer.empty()) {
        finish(false, tr("No response from daemon (connection closed)."));
        return;
    }
    ds4ipc::trim_response(&m_buffer);
    finish(true, QString::fromStdString(m_buffer));
}

void IpcClient::onTimeout() {
    if (!m_inFlight) {
        return;
    }
    finish(false, tr("Timed out after %1 ms waiting for the daemon.")
                      .arg(ds4ipc::kDefaultTimeoutMs));
}

void IpcClient::closeConnection() {
    m_timeout->stop();
    if (m_notifier) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
}

void IpcClient::finish(bool ok, const QString &response) {
    closeConnection();
    m_inFlight = false;
    const QString command = m_current;
    m_current.clear();
    m_buffer.clear();
    emit replyReady(command, ok, response);
    startNext();
}
