#ifndef DS4_UI_DAEMONCONTROLLER_H
#define DS4_UI_DAEMONCONTROLLER_H

#include <QObject>
#include <QStringList>
#include <QQmlEngine>
#include <QString>
#include <QTimer>

#include <string>

#include "ipcclient.h"

// Model behind the settings window: mirrors the daemon's `status` output as
// QML-readable properties and exposes the CLI's `set-*` commands as
// invokables.
//
// It holds no knowledge of the protocol itself -- accepted values, command
// strings, name limits and status parsing all come from ds4ipc
// (src/ipc-client.h), the same code ds4-ctl runs, so the two front-ends
// cannot disagree about what the daemon accepts.
class DaemonController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool online READ online NOTIFY statusChanged)
    // True only while a command the *user* triggered is in flight. Background
    // status polls deliberately do not set it: they must never disable a
    // control the user is in the middle of using.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString lastMessage READ lastMessage NOTIFY lastMessageChanged)
    Q_PROPERTY(bool lastMessageIsError READ lastMessageIsError NOTIFY lastMessageChanged)

    Q_PROPERTY(QString physicalController READ physicalController NOTIFY statusChanged)
    Q_PROPERTY(QString connectionType READ connectionType NOTIFY statusChanged)
    Q_PROPERTY(QString activeBackend READ activeBackend NOTIFY statusChanged)
    // Config-string values ("ds4"/"dualsense"/"none"/"hidden",
    // "uhid"/"functionfs", "legacy"/"unbind") so QML can bind them straight
    // to a control's current index.
    Q_PROPERTY(QString emulationType READ emulationType NOTIFY statusChanged)
    Q_PROPERTY(QString ds4Backend READ ds4Backend NOTIFY statusChanged)
    Q_PROPERTY(QString dualsenseBackend READ dualsenseBackend NOTIFY statusChanged)
    Q_PROPERTY(QString hideMethod READ hideMethod NOTIFY statusChanged)
    Q_PROPERTY(QString ds4Name READ ds4Name NOTIFY statusChanged)
    Q_PROPERTY(QString dualsenseName READ dualsenseName NOTIFY statusChanged)
    Q_PROPERTY(bool ds4NameIsDefault READ ds4NameIsDefault NOTIFY statusChanged)
    Q_PROPERTY(bool dualsenseNameIsDefault READ dualsenseNameIsDefault NOTIFY statusChanged)

public:
    explicit DaemonController(QObject *parent = nullptr);

    bool online() const { return m_online; }
    bool busy() const { return m_pendingCommands > 0; }
    QString lastMessage() const { return m_lastMessage; }
    bool lastMessageIsError() const { return m_lastMessageIsError; }

    QString physicalController() const { return m_physicalController; }
    QString connectionType() const { return m_connectionType; }
    QString activeBackend() const { return m_activeBackend; }
    QString emulationType() const { return m_emulationType; }
    QString ds4Backend() const { return m_ds4Backend; }
    QString dualsenseBackend() const { return m_dualsenseBackend; }
    QString hideMethod() const { return m_hideMethod; }
    QString ds4Name() const { return m_ds4Name; }
    QString dualsenseName() const { return m_dualsenseName; }
    bool ds4NameIsDefault() const { return m_ds4NameIsDefault; }
    bool dualsenseNameIsDefault() const { return m_dualsenseNameIsDefault; }

    Q_INVOKABLE void refreshStatus();

    // The values the daemon accepts, straight from ds4ipc, so the controls
    // offer exactly what ds4-ctl offers. Front-end labels are looked up by
    // value in QML rather than positionally, so this list stays free to
    // change order.
    Q_INVOKABLE QStringList typeValues() const;
    Q_INVOKABLE QStringList backendValues() const;
    Q_INVOKABLE QStringList hideMethodValues() const;

    // type: ds4 | dualsense | none | hidden
    Q_INVOKABLE void setType(const QString &type);
    // controller: ds4 | dualsense -- backend: uhid | functionfs
    Q_INVOKABLE void setBackend(const QString &controller, const QString &backend);
    // controller: ds4 | dualsense -- name: free text, length-checked by ds4ipc
    Q_INVOKABLE void setName(const QString &controller, const QString &name);
    Q_INVOKABLE void resetName(const QString &controller);
    // method: legacy | unbind
    Q_INVOKABLE void setHideMethod(const QString &method);

    // Hardware tests, not settings: they touch the physical controller
    // directly and the daemon hands it straight back to the emulated device
    // afterwards. They deliberately do not count as commands in flight -- the
    // window must not grey itself out because a colour slider moved.
    Q_INVOKABLE void identify();
    Q_INVOKABLE void testLed(int red, int green, int blue, int rumbleLeft, int rumbleRight);
    Q_INVOKABLE void resetLed();

    // Returns an empty string if the name is acceptable, otherwise the reason
    // it is not -- so QML can disable the Apply button before sending.
    Q_INVOKABLE QString validateName(const QString &name) const;

    // Bracket a direct interaction (an open combo box popup, say) to hold off
    // background polling: a status refresh landing mid-interaction would move
    // the control under the user's cursor. Reference-counted, so overlapping
    // interactions are safe.
    Q_INVOKABLE void beginInteraction();
    Q_INVOKABLE void endInteraction();

signals:
    void statusChanged();
    void busyChanged();
    void lastMessageChanged();

private slots:
    void onReplyReady(const QString &command, bool ok, const QString &response);

private:
    void sendCommand(const QString &command);
    void sendBuiltCommand(const std::string &command, const std::string &error);
    void setPendingCommands(int count);
    void applyStatus(const QString &response);
    void setMessage(const QString &message, bool isError);
    void setOnline(bool online);
    bool requireControllerArg(const QString &controller);

    IpcClient *m_ipc;
    QTimer *m_pollTimer;

    bool m_online = false;
    int m_pendingCommands = 0;
    int m_interactions = 0;
    bool m_lastMessageIsError = false;
    QString m_lastMessage;
    // Scratch space for the ds4ipc builders' error output.
    std::string m_buildError;

    QString m_physicalController;
    QString m_connectionType;
    QString m_activeBackend;
    QString m_emulationType;
    QString m_ds4Backend;
    QString m_dualsenseBackend;
    QString m_hideMethod;
    QString m_ds4Name;
    QString m_dualsenseName;
    bool m_ds4NameIsDefault = true;
    bool m_dualsenseNameIsDefault = true;
};

#endif // DS4_UI_DAEMONCONTROLLER_H
