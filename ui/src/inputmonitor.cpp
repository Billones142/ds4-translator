#include "inputmonitor.h"

#include <unistd.h>

namespace {

// The daemon is not always running (or not yet), and a controller may be
// plugged in long after the view is opened, so a dropped stream is retried
// rather than reported as fatal.
constexpr int kRetryIntervalMs = 2000;

// How many one-second windows a measurement averages.
constexpr int kMeasureWindows = 3;

} // namespace

InputMonitor::InputMonitor(QObject *parent)
    : QObject(parent), m_retry(new QTimer(this)),
      m_connectionNote(tr("Not watching the controller.")) {
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
        setConnectionNote(tr("Connecting to the daemon…"));
        tryConnect();
        m_retry->start();
    } else {
        m_retry->stop();
        disconnectStream();
        setConnectionNote(tr("Not watching the controller."));
    }
}

void InputMonitor::setSource(Source source) {
    if (m_source == source) {
        return;
    }
    m_source = source;
    emit sourceChanged();
    // Everything the view reads comes from the selected source, so switching
    // is the same event as new data arriving.
    emit inputChanged();
    emit noteChanged();
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
        setConnectionNote(QString::fromStdString(error));
        return;
    }
    // "all": both sources every tick, since the view lets the user switch
    // between them without reconnecting. An older daemon rejects it, and the
    // reply below downgrades this to the single-source command.
    const char *command = m_allSources ? ds4ipc::kTestAllCommand : ds4ipc::kTestCommand;
    if (!ds4ipc::write_command(fd, command, &error) ||
        !ds4ipc::set_non_blocking(fd, &error)) {
        close(fd);
        setConnectionNote(QString::fromStdString(error));
        return;
    }

    m_fd = fd;
    m_buffer.clear();
    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &InputMonitor::onReadable);
    setConnectionNote(tr("Waiting for data from the daemon…"));
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
    if (m_hasOutput) {
        m_hasOutput = false;
        emit outputChanged();
    }
    clearStates();
}

void InputMonitor::clearStates() {
    bool had_state = false;
    for (int i = 0; i < kSourceCount; ++i) {
        had_state = had_state || m_hasState[i];
        m_hasState[i] = false;
        m_notes[i].clear();
    }
    if (had_state) {
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
        // A daemon older than "test all" rejects it and closes, and its reply
        // carries no trailing newline, so it is still sitting in the buffer
        // rather than having gone through handleLine().
        const bool unsupported = status == ds4ipc::ReadStatus::Eof && m_allSources &&
                                 m_buffer.rfind("Unknown command", 0) == 0;
        disconnectStream();
        if (unsupported) {
            // The retry timer reconnects in a moment with the single-source
            // command, which every daemon version understands.
            m_allSources = false;
            setConnectionNote(tr("This daemon only streams its active source. Restart "
                                 "ds4-translator after updating it to watch both."));
        } else {
            setConnectionNote(status == ds4ipc::ReadStatus::Eof
                                  ? tr("The daemon closed the connection.")
                                  : QString::fromStdString(error));
        }
        return; // the retry timer picks it up again
    }

    std::string::size_type newline = 0;
    while ((newline = m_buffer.find('\n')) != std::string::npos) {
        handleLine(m_buffer.substr(0, newline));
        m_buffer.erase(0, newline + 1);
    }
}

int InputMonitor::sourceIndex(const std::string &source) {
    if (source == ds4ipc::kSourceVirtual) {
        return Virtual;
    }
    if (source == ds4ipc::kSourcePhysical) {
        return Physical;
    }
    return -1;
}

void InputMonitor::measure() {
    m_accumulated = ds4ipc::TimingStats();
    m_measureWindows = kMeasureWindows;
    m_measured = false;
    emit timingChanged();
}

double InputMonitor::reportRate() const {
    return m_result.reports_per_second();
}

void InputMonitor::applyTiming(const ds4ipc::TimingStats &window) {
    if (!m_timingSupported) {
        m_timingSupported = true;
        emit timingChanged();
    }
    if (m_measureWindows <= 0) {
        return;
    }
    // Windows are summed rather than averaged one by one: the mean interval
    // of the whole span is the total time divided by the total number of
    // reports, which a mean of means only matches when every window happens
    // to carry the same number of them.
    m_accumulated.reports += window.reports;
    m_accumulated.window_us += window.window_us;
    m_accumulated.interval_mean_us += window.interval_mean_us * window.reports;
    if (window.reports > 0 && (m_accumulated.interval_min_us == 0 ||
                               window.interval_min_us < m_accumulated.interval_min_us)) {
        m_accumulated.interval_min_us = window.interval_min_us;
    }
    if (window.interval_max_us > m_accumulated.interval_max_us) {
        m_accumulated.interval_max_us = window.interval_max_us;
    }
    m_accumulated.latency_mean_us += window.latency_mean_us * window.reports;
    if (window.latency_max_us > m_accumulated.latency_max_us) {
        m_accumulated.latency_max_us = window.latency_max_us;
    }

    if (--m_measureWindows > 0) {
        emit timingChanged();
        return;
    }

    m_result = m_accumulated;
    if (m_accumulated.reports > 0) {
        m_result.interval_mean_us = m_accumulated.interval_mean_us / m_accumulated.reports;
        m_result.latency_mean_us = m_accumulated.latency_mean_us / m_accumulated.reports;
    } else {
        m_result.interval_mean_us = 0;
        m_result.latency_mean_us = 0;
    }
    m_measured = true;
    emit timingChanged();
}

void InputMonitor::handleLine(const std::string &line) {
    ds4ipc::OutputState output;
    if (ds4ipc::parse_output(line, &output)) {
        m_output = output;
        m_hasOutput = true;
        emit outputChanged();
        return;
    }

    ds4ipc::TimingStats window;
    if (ds4ipc::parse_timing(line, &window)) {
        applyTiming(window);
        return;
    }

    ds4ipc::InputState parsed;
    if (ds4ipc::parse_state(line, &parsed)) {
        const int index = sourceIndex(parsed.source);
        if (index < 0) {
            return; // a source this build does not know about
        }
        m_states[index] = parsed;
        m_hasState[index] = true;
        // Emitted for either source: the picker shows both as available or
        // not, so the other source's arrival is a visible change too.
        emit inputChanged();
        return;
    }

    std::string note_source;
    std::string note_text;
    if (ds4ipc::parse_note(line, &note_source, &note_text)) {
        // The daemon has nothing to show for that source (no controller, no
        // virtual device yet, emulation off) and says why; keep displaying
        // its reason instead of a stale snapshot.
        const QString text = QString::fromStdString(note_text);
        const int index = sourceIndex(note_source);
        if (index < 0) {
            // A daemon older than the two-source stream does not name the
            // source in its notes. It only follows one source anyway, so the
            // reason applies to whatever this view could have shown.
            for (int i = 0; i < kSourceCount; ++i) {
                if (m_hasState[i]) {
                    m_hasState[i] = false;
                    emit inputChanged();
                }
                setNote(i, text);
            }
            return;
        }
        if (m_hasState[index]) {
            m_hasState[index] = false;
            emit inputChanged();
        }
        setNote(index, text);
    }
    // OK:/CAVEAT/EVENT lines are about the subscription and the LED/rumble
    // traffic; the terminal monitor shows them, this view does not.
}

QString InputMonitor::note() const {
    // While the stream is down there is nothing per-source to say -- the
    // reason is the connection itself.
    if (m_fd < 0) {
        return m_connectionNote;
    }
    const QString &note = m_notes[m_source];
    return note.isEmpty() ? m_connectionNote : note;
}

void InputMonitor::setNote(int source, const QString &note) {
    if (m_notes[source] == note) {
        return;
    }
    m_notes[source] = note;
    if (source == m_source) {
        emit noteChanged();
    }
}

void InputMonitor::setConnectionNote(const QString &note) {
    if (m_connectionNote == note) {
        return;
    }
    m_connectionNote = note;
    emit noteChanged();
}

QString InputMonitor::dpadName() const {
    return QString::fromLatin1(ds4ipc::dpad_name(selected().dpad));
}
