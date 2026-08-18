#include "inputmonitor.h"

#include <unistd.h>

namespace {

// The daemon is not always running (or not yet), and a controller may be
// plugged in long after the view is opened, so a dropped stream is retried
// rather than reported as fatal.
constexpr int kRetryIntervalMs = 2000;

} // namespace

InputMonitor::InputMonitor(QObject *parent)
    : QObject(parent), m_retry(new QTimer(this)),
      m_note(tr("Not watching the controller.")) {
    m_retry->setInterval(kRetryIntervalMs);
    connect(m_retry, &QTimer::timeout, this, &InputMonitor::tryConnect);
}

InputMonitor::~InputMonitor() {
    disconnectStream();
}

void InputMonitor::setActive(bool active) {
    if (m_active == active) {
        return;
    }
    m_active = active;
    emit activeChanged();

    if (m_active) {
        setNote(tr("Connecting to the daemon…"));
        tryConnect();
        m_retry->start();
    } else {
        m_retry->stop();
        disconnectStream();
        setNote(tr("Not watching the controller."));
    }
}

void InputMonitor::tryConnect() {
    if (!m_active || m_fd >= 0) {
        return;
    }

    std::string error;
    // Blocking connect + write (one short line), then the read side goes
    // async -- the same split IpcClient uses for the settings commands.
    int fd = ds4ipc::connect_socket(&error);
    if (fd < 0) {
        setNote(QString::fromStdString(error));
        return;
    }
    if (!ds4ipc::write_command(fd, ds4ipc::kTestCommand, &error) ||
        !ds4ipc::set_non_blocking(fd, &error)) {
        close(fd);
        setNote(QString::fromStdString(error));
        return;
    }

    m_fd = fd;
    m_buffer.clear();
    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &InputMonitor::onReadable);
    setNote(tr("Waiting for data from the daemon…"));
    emit connectedChanged();
}

void InputMonitor::disconnectStream() {
    if (m_notifier != nullptr) {
        m_notifier->setEnabled(false);
        m_notifier->deleteLater();
        m_notifier = nullptr;
    }
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
        emit connectedChanged();
    }
    m_buffer.clear();
    if (m_hasState) {
        m_hasState = false;
        emit inputChanged();
    }
}

void InputMonitor::onReadable() {
    std::string error;
    ds4ipc::ReadStatus status = ds4ipc::read_available(m_fd, &m_buffer, &error);
    if (status == ds4ipc::ReadStatus::Again) {
        return;
    }
    if (status == ds4ipc::ReadStatus::Eof || status == ds4ipc::ReadStatus::Error) {
        disconnectStream();
        setNote(status == ds4ipc::ReadStatus::Eof
                    ? tr("The daemon closed the connection.")
                    : QString::fromStdString(error));
        return; // the retry timer picks it up again
    }

    std::string::size_type newline = 0;
    while ((newline = m_buffer.find('\n')) != std::string::npos) {
        handleLine(m_buffer.substr(0, newline));
        m_buffer.erase(0, newline + 1);
    }
}

void InputMonitor::handleLine(const std::string &line) {
    ds4ipc::InputState parsed;
    if (ds4ipc::parse_state(line, &parsed)) {
        m_state = parsed;
        m_hasState = true;
        emit inputChanged();
        return;
    }
    if (line.rfind("NOTE ", 0) == 0) {
        // The daemon has nothing to show (no controller, or no virtual device
        // yet) and says why; keep displaying its reason instead of a stale
        // snapshot.
        if (m_hasState) {
            m_hasState = false;
            emit inputChanged();
        }
        setNote(QString::fromStdString(line.substr(5)));
    }
    // OK:/CAVEAT/EVENT lines are about the subscription and the LED/rumble
    // traffic; the terminal monitor shows them, this view does not.
}

void InputMonitor::setNote(const QString &note) {
    if (m_note == note) {
        return;
    }
    m_note = note;
    emit noteChanged();
}

QString InputMonitor::dpadName() const {
    return QString::fromLatin1(ds4ipc::dpad_name(m_state.dpad));
}
