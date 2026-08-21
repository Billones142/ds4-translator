#include "daemoncontroller.h"

#include <QStringList>

#include "ipc-client.h"

namespace {

// How often the window re-reads `status` so external changes (ds4-ctl from a
// terminal, a controller being plugged in) show up without user action.
constexpr int kPollIntervalMs = 2000;

// The one command the UI sends on its own, without the user asking.
constexpr auto kStatusCommand = QLatin1String("status");

// How the daemon marks a name it made up itself in its status report.
constexpr auto kDefaultNameSuffix = QLatin1String(" (default)");

// Commands that poke the hardware for a moment (see the invokables below).
// Their replies must not queue a status refresh: the daemon's state has not
// changed, and a colour slider would otherwise fire a poll per step.
bool isHardwareTest(const QString &command) {
    return command == QLatin1String(ds4ipc::kIdentifyCommand) ||
           command.startsWith(QLatin1String("led"));
}

QStringList toStringList(const std::vector<std::string> &values) {
    QStringList list;
    list.reserve(static_cast<qsizetype>(values.size()));
    for (const std::string &value : values) {
        list.append(QString::fromStdString(value));
    }
    return list;
}

} // namespace

DaemonController::DaemonController(QObject *parent)
    : QObject(parent), m_ipc(new IpcClient(this)), m_pollTimer(new QTimer(this)) {
    connect(m_ipc, &IpcClient::replyReady, this, &DaemonController::onReplyReady);

    m_pollTimer->setInterval(kPollIntervalMs);
    connect(m_pollTimer, &QTimer::timeout, this, &DaemonController::refreshStatus);
    m_pollTimer->start();

    refreshStatus();
}

void DaemonController::sendCommand(const QString &command) {
    if (command != kStatusCommand && !isHardwareTest(command)) {
        setPendingCommands(m_pendingCommands + 1);
    }
    m_ipc->send(command);
}

void DaemonController::setPendingCommands(int count) {
    if (m_pendingCommands == count) {
        return;
    }
    const bool wasBusy = busy();
    m_pendingCommands = count;
    if (wasBusy != busy()) {
        emit busyChanged();
    }
}

void DaemonController::beginInteraction() {
    ++m_interactions;
}

void DaemonController::endInteraction() {
    if (m_interactions > 0) {
        --m_interactions;
    }
}

void DaemonController::refreshStatus() {
    // Skip the tick while the socket is busy: the refresh that follows every
    // completed command already covers that case, and queueing polls behind a
    // slow request (set-type recreates the virtual device and can take
    // seconds) would only pile up work.
    if (m_ipc->isBusy()) {
        return;
    }
    // Skip it while the user is interacting with a control too, so a refresh
    // cannot move the selection out from under them.
    if (m_interactions > 0) {
        return;
    }
    sendCommand(kStatusCommand);
}

QStringList DaemonController::typeValues() const {
    return toStringList(ds4ipc::type_values());
}

QStringList DaemonController::backendValues() const {
    return toStringList(ds4ipc::backend_values());
}

QStringList DaemonController::hideMethodValues() const {
    return toStringList(ds4ipc::hide_method_values());
}

void DaemonController::setType(const QString &type) {
    sendBuiltCommand(ds4ipc::build_set_type(type.toStdString(), &m_buildError), m_buildError);
}

void DaemonController::setBackend(const QString &controller, const QString &backend) {
    sendBuiltCommand(
        ds4ipc::build_set_backend(controller.toStdString(), backend.toStdString(), &m_buildError),
        m_buildError);
}

QString DaemonController::validateName(const QString &name) const {
    return QString::fromStdString(ds4ipc::validate_name(name.toStdString()));
}

void DaemonController::setName(const QString &controller, const QString &name) {
    sendBuiltCommand(
        ds4ipc::build_set_name(controller.toStdString(), name.toStdString(), &m_buildError),
        m_buildError);
}

void DaemonController::resetName(const QString &controller) {
    sendBuiltCommand(ds4ipc::build_set_name(controller.toStdString(), "--reset", &m_buildError),
                     m_buildError);
}

void DaemonController::identify() {
    sendCommand(QString::fromLatin1(ds4ipc::kIdentifyCommand));
}

void DaemonController::testLed(int red, int green, int blue, int rumbleLeft, int rumbleRight) {
    sendBuiltCommand(ds4ipc::build_led(red, green, blue, rumbleLeft, rumbleRight, &m_buildError),
                     m_buildError);
}

void DaemonController::resetLed() {
    sendCommand(QString::fromLatin1(ds4ipc::kLedResetCommand));
}

void DaemonController::setHideMethod(const QString &method) {
    sendBuiltCommand(ds4ipc::build_set_hide_method(method.toStdString(), &m_buildError),
                     m_buildError);
}

// Every setter funnels through here: ds4ipc decides whether the arguments are
// acceptable and what the command looks like, so the UI cannot accept an
// argument ds4-ctl would reject, or word the rejection differently.
void DaemonController::sendBuiltCommand(const std::string &command, const std::string &error) {
    if (command.empty()) {
        setMessage(QString::fromStdString(error), true);
        return;
    }
    sendCommand(QString::fromStdString(command));
}

void DaemonController::onReplyReady(const QString &command, bool ok, const QString &response) {
    const bool isStatus = (command == kStatusCommand);
    const bool isTest = isHardwareTest(command);
    if (!isStatus && !isTest) {
        setPendingCommands(m_pendingCommands - 1);
    }

    if (!ok) {
        setOnline(false);
        // A failed poll would otherwise overwrite the error text of the
        // command the user actually ran, once per poll interval.
        if (!isStatus || m_lastMessage.isEmpty()) {
            setMessage(response, true);
        }
    } else if (isTest) {
        setOnline(true);
        // Nothing on the daemon changed, so no status refresh. A successful
        // colour change is visible on the controller itself and needs no
        // banner; a refusal (no controller connected) does.
        const bool failed = response.startsWith(QLatin1String("Error"));
        if (failed || command == QLatin1String(ds4ipc::kIdentifyCommand)) {
            setMessage(response, failed);
        }
    } else if (isStatus) {
        setOnline(true);
        applyStatus(response);
    } else {
        setOnline(true);
        setMessage(response, response.startsWith(QLatin1String("Error")));
        // Re-read the daemon's own view instead of assuming the command took
        // effect: set-type can be refused (e.g. while a physical controller
        // is connected) and still return a plain message.
        sendCommand(kStatusCommand);
    }
}

void DaemonController::applyStatus(const QString &response) {
    const std::vector<ds4ipc::StatusField> fields = ds4ipc::parse_status(response.toStdString());
    const auto field = [&fields](const char *key) {
        return QString::fromStdString(ds4ipc::status_value(fields, key));
    };

    m_physicalController = field("Physical Controller");
    m_connectionType = field("Connection Type");
    m_activeBackend = field("Active Backend");
    m_emulationType = QString::fromStdString(
        ds4ipc::type_display_to_config(field("Virtual Emulation").toStdString()));
    m_ds4Backend = field("DS4 Backend");
    m_dualsenseBackend = field("DualSense Backend");
    // "Hide Method" may carry a trailing "(active on current connection)".
    m_hideMethod = field("Hide Method").section(QLatin1Char(' '), 0, 0);

    const QString ds4Name = field("DS4 Name");
    m_ds4NameIsDefault = ds4Name.endsWith(kDefaultNameSuffix);
    m_ds4Name = m_ds4NameIsDefault ? ds4Name.chopped(kDefaultNameSuffix.size()) : ds4Name;

    const QString dualsenseName = field("DualSense Name");
    m_dualsenseNameIsDefault = dualsenseName.endsWith(kDefaultNameSuffix);
    m_dualsenseName = m_dualsenseNameIsDefault
                          ? dualsenseName.chopped(kDefaultNameSuffix.size())
                          : dualsenseName;

    emit statusChanged();
}

void DaemonController::setMessage(const QString &message, bool isError) {
    if (m_lastMessage == message && m_lastMessageIsError == isError) {
        return;
    }
    m_lastMessage = message;
    m_lastMessageIsError = isError;
    emit lastMessageChanged();
}

void DaemonController::setOnline(bool online) {
    if (m_online == online) {
        return;
    }
    m_online = online;
    if (online) {
        // Drop a stale "daemon unreachable" banner as soon as it answers.
        if (m_lastMessageIsError) {
            setMessage(QString(), false);
        }
    }
    emit statusChanged();
}
