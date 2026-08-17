#include "daemoncontroller.h"

#include <QStringList>

namespace {

// How often the window re-reads `status` so external changes (ds4-ctl from a
// terminal, a controller being plugged in) show up without user action.
constexpr int kPollIntervalMs = 2000;

// Accepted argument values, mirroring ds4-ctl's own tables. Built on call
// rather than held in namespace-scope QStringLists: a container with static
// storage duration would construct before main() with no way to catch a
// throw (bugprone-throwing-static-initialization), and these lists are tiny.
QStringList controllerValues() {
    return {QStringLiteral("ds4"), QStringLiteral("dualsense")};
}
QStringList typeValues() {
    return {QStringLiteral("ds4"), QStringLiteral("dualsense"), QStringLiteral("none"),
            QStringLiteral("hidden")};
}
QStringList backendValues() {
    return {QStringLiteral("uhid"), QStringLiteral("functionfs")};
}
QStringList hideMethodValues() {
    return {QStringLiteral("legacy"), QStringLiteral("unbind")};
}

// The daemon reports display names ("DualShock 4"); the set-* commands take
// config strings ("ds4"). Map one to the other so a status refresh can drive
// the same controls the setters write to.
QString typeDisplayToConfig(const QString &display) {
    if (display == QLatin1String("DualShock 4")) return QStringLiteral("ds4");
    if (display == QLatin1String("DualSense")) return QStringLiteral("dualsense");
    if (display == QLatin1String("Hidden")) return QStringLiteral("hidden");
    return QStringLiteral("none");
}

// "Hide Method" may carry a trailing "(active on current connection)" note.
QString firstToken(const QString &value) {
    return value.section(' ', 0, 0);
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
    const bool wasBusy = busy();
    m_ipc->send(command);
    if (!wasBusy) {
        emit busyChanged();
    }
}

void DaemonController::refreshStatus() {
    // Skip the tick if a command is still in flight: the refresh that follows
    // every completed command already covers that case, and queueing polls
    // behind a stuck request would only pile up work.
    if (busy()) {
        return;
    }
    sendCommand(QStringLiteral("status"));
}

void DaemonController::setType(const QString &type) {
    if (!typeValues().contains(type)) {
        setMessage(tr("Invalid emulation type '%1'. Supported: %2")
                       .arg(type, typeValues().join(QLatin1Char(' '))),
                   true);
        return;
    }
    sendCommand(QStringLiteral("set-type ") + type);
}

void DaemonController::setBackend(const QString &controller, const QString &backend) {
    if (!requireControllerArg(controller)) {
        return;
    }
    if (!backendValues().contains(backend)) {
        setMessage(tr("Invalid backend '%1'. Supported: %2")
                       .arg(backend, backendValues().join(QLatin1Char(' '))),
                   true);
        return;
    }
    sendCommand(QStringLiteral("set-backend ") + controller + QLatin1Char(' ') + backend);
}

QString DaemonController::validateName(const QString &name) const {
    if (name.isEmpty()) {
        return tr("Name cannot be empty (use Reset to restore the default).");
    }
    if (name.contains(QLatin1Char('\n')) || name.contains(QLatin1Char('\r'))) {
        return tr("Name cannot contain newline characters.");
    }
    const qsizetype bytes = name.toUtf8().size();
    if (bytes > kMaxNameBytes) {
        return tr("Name too long (%1 bytes, max %2).").arg(bytes).arg(kMaxNameBytes);
    }
    return QString();
}

void DaemonController::setName(const QString &controller, const QString &name) {
    if (!requireControllerArg(controller)) {
        return;
    }
    const QString problem = validateName(name);
    if (!problem.isEmpty()) {
        setMessage(problem, true);
        return;
    }
    sendCommand(QStringLiteral("set-name ") + controller + QLatin1Char(' ') + name);
}

void DaemonController::resetName(const QString &controller) {
    if (!requireControllerArg(controller)) {
        return;
    }
    sendCommand(QStringLiteral("set-name ") + controller + QStringLiteral(" --reset"));
}

void DaemonController::setHideMethod(const QString &method) {
    if (!hideMethodValues().contains(method)) {
        setMessage(tr("Invalid hide method '%1'. Supported: %2")
                       .arg(method, hideMethodValues().join(QLatin1Char(' '))),
                   true);
        return;
    }
    sendCommand(QStringLiteral("set-hide-method ") + method);
}

bool DaemonController::requireControllerArg(const QString &controller) {
    if (controllerValues().contains(controller)) {
        return true;
    }
    setMessage(tr("Invalid controller '%1'. Supported: %2")
                   .arg(controller, controllerValues().join(QLatin1Char(' '))),
               true);
    return false;
}

void DaemonController::onReplyReady(const QString &command, bool ok, const QString &response) {
    const bool isStatus = (command == QLatin1String("status"));

    if (!ok) {
        setOnline(false);
        // A failed poll would otherwise overwrite the error text of the
        // command the user actually ran, once per poll interval.
        if (!isStatus || m_lastMessage.isEmpty()) {
            setMessage(response, true);
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
        sendCommand(QStringLiteral("status"));
    }

    if (!busy()) {
        emit busyChanged();
    }
}

void DaemonController::applyStatus(const QString &response) {
    const QStringList lines = response.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const qsizetype sep = line.indexOf(QLatin1Char(':'));
        if (sep < 0) {
            continue;
        }
        const QString key = line.left(sep).trimmed();
        const QString value = line.mid(sep + 1).trimmed();

        if (key == QLatin1String("Physical Controller")) {
            m_physicalController = value;
        } else if (key == QLatin1String("Connection Type")) {
            m_connectionType = value;
        } else if (key == QLatin1String("Active Backend")) {
            m_activeBackend = value;
        } else if (key == QLatin1String("Virtual Emulation")) {
            m_emulationType = typeDisplayToConfig(value);
        } else if (key == QLatin1String("DS4 Backend")) {
            m_ds4Backend = value;
        } else if (key == QLatin1String("DualSense Backend")) {
            m_dualsenseBackend = value;
        } else if (key == QLatin1String("Hide Method")) {
            m_hideMethod = firstToken(value);
        } else if (key == QLatin1String("DS4 Name")) {
            m_ds4NameIsDefault = value.endsWith(QLatin1String("(default)"));
            m_ds4Name = m_ds4NameIsDefault ? value.chopped(QStringLiteral(" (default)").size())
                                           : value;
        } else if (key == QLatin1String("DualSense Name")) {
            m_dualsenseNameIsDefault = value.endsWith(QLatin1String("(default)"));
            m_dualsenseName = m_dualsenseNameIsDefault
                                  ? value.chopped(QStringLiteral(" (default)").size())
                                  : value;
        }
    }
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
