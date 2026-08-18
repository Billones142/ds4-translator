#ifndef DS4_UI_INPUTMONITOR_H
#define DS4_UI_INPUTMONITOR_H

#include <QObject>
#include <QQmlEngine>
#include <QSocketNotifier>
#include <QString>
#include <QTimer>

#include <string>

#include "ipc-client.h"

// Live view of whatever the daemon currently sees on the controller.
//
// This is the `test` command, the one command whose connection stays open:
// the daemon promotes the socket into its broadcast list and pushes a STATE
// line about 30 times a second, so unlike the settings commands there is no
// request/response pairing here and no IpcClient queue involved.
//
// The stream is only subscribed to while something is actually watching it
// (setActive(false) drops the connection): a subscriber makes the daemon poll
// its input source far more aggressively, and a hidden window has no reason
// to cost it that.
class InputMonitor : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    // Set by the view that displays this: true only while it is on screen.
    Q_PROPERTY(bool active READ isActive WRITE setActive NOTIFY activeChanged)
    // Connected to the daemon's stream (which does not yet mean a controller
    // is reporting anything -- see `hasState`).
    Q_PROPERTY(bool connected READ isConnected NOTIFY connectedChanged)
    // True once a STATE line arrived; false while the daemon is only sending
    // a NOTE (no controller, or no virtual device yet).
    Q_PROPERTY(bool hasState READ hasState NOTIFY inputChanged)
    // Why there is no state, in the daemon's words, or the connection error.
    Q_PROPERTY(QString note READ note NOTIFY noteChanged)
    // Which device the values come from: "VIRTUAL" (the emulated controller)
    // or "PHYSICAL" (raw passthrough, when emulation is off).
    Q_PROPERTY(QString source READ source NOTIFY inputChanged)

    // Axes as the controller reports them: sticks 0..255 with 128 centred,
    // triggers 0..255 with 0 released.
    Q_PROPERTY(int leftX READ leftX NOTIFY inputChanged)
    Q_PROPERTY(int leftY READ leftY NOTIFY inputChanged)
    Q_PROPERTY(int rightX READ rightX NOTIFY inputChanged)
    Q_PROPERTY(int rightY READ rightY NOTIFY inputChanged)
    Q_PROPERTY(int l2 READ l2 NOTIFY inputChanged)
    Q_PROPERTY(int r2 READ r2 NOTIFY inputChanged)

    // Pressed buttons, as a mask of Button values.
    Q_PROPERTY(int buttons READ buttons NOTIFY inputChanged)
    // D-pad hat: 0 = up, clockwise in 45 degree steps, 8 = neutral.
    Q_PROPERTY(int dpad READ dpad NOTIFY inputChanged)

    // False when the daemon is older than the motion fields of the STATE
    // line, so the sensor values below are zeros and not readings.
    Q_PROPERTY(bool hasMotion READ hasMotion NOTIFY inputChanged)
    // Raw motion sensor counts. Deliberately not converted to degrees/s or g:
    // that needs the per-unit calibration data the daemon does not forward.
    Q_PROPERTY(int gyroPitch READ gyroPitch NOTIFY inputChanged)
    Q_PROPERTY(int gyroYaw READ gyroYaw NOTIFY inputChanged)
    Q_PROPERTY(int gyroRoll READ gyroRoll NOTIFY inputChanged)
    Q_PROPERTY(int accelX READ accelX NOTIFY inputChanged)
    Q_PROPERTY(int accelY READ accelY NOTIFY inputChanged)
    Q_PROPERTY(int accelZ READ accelZ NOTIFY inputChanged)

public:
    explicit InputMonitor(QObject *parent = nullptr);
    ~InputMonitor() override;

    // Mirrors ds4ipc::Button so QML can name a bit (InputMonitor.Cross)
    // instead of repeating the numbers.
    enum Button {
        Square = ds4ipc::kBtnSquare,
        Cross = ds4ipc::kBtnCross,
        Circle = ds4ipc::kBtnCircle,
        Triangle = ds4ipc::kBtnTriangle,
        L1 = ds4ipc::kBtnL1,
        R1 = ds4ipc::kBtnR1,
        L2 = ds4ipc::kBtnL2,
        R2 = ds4ipc::kBtnR2,
        Share = ds4ipc::kBtnShare,
        Options = ds4ipc::kBtnOptions,
        L3 = ds4ipc::kBtnL3,
        R3 = ds4ipc::kBtnR3,
        Ps = ds4ipc::kBtnPs,
        Touchpad = ds4ipc::kBtnTouchpad,
    };
    Q_ENUM(Button)

    bool isActive() const { return m_active; }
    void setActive(bool active);

    bool isConnected() const { return m_fd >= 0; }
    bool hasState() const { return m_hasState; }
    QString note() const { return m_note; }
    QString source() const { return QString::fromStdString(m_state.source); }

    int leftX() const { return m_state.lx; }
    int leftY() const { return m_state.ly; }
    int rightX() const { return m_state.rx; }
    int rightY() const { return m_state.ry; }
    int l2() const { return m_state.l2; }
    int r2() const { return m_state.r2; }
    bool hasMotion() const { return m_state.has_motion; }
    int buttons() const { return m_state.buttons; }
    int dpad() const { return m_state.dpad; }
    int gyroPitch() const { return m_state.gyro[0]; }
    int gyroYaw() const { return m_state.gyro[1]; }
    int gyroRoll() const { return m_state.gyro[2]; }
    int accelX() const { return m_state.accel[0]; }
    int accelY() const { return m_state.accel[1]; }
    int accelZ() const { return m_state.accel[2]; }

    // True when `bit` (a Button value) is currently held.
    Q_INVOKABLE bool isPressed(int bit) const { return (m_state.buttons & bit) != 0; }
    // "Up", "Down-Left", "Neutral", ... for the current hat value.
    Q_INVOKABLE QString dpadName() const;

signals:
    void activeChanged();
    void connectedChanged();
    void noteChanged();
    // One signal for the whole input snapshot: every field of a STATE line
    // changes together, ~30 times a second, so splitting it into a signal per
    // property would only multiply the work with no gain.
    void inputChanged();

private slots:
    void onReadable();
    void tryConnect();

private:
    void disconnectStream();
    void handleLine(const std::string &line);
    void setNote(const QString &note);

    QTimer *m_retry;
    QSocketNotifier *m_notifier = nullptr;
    ds4ipc::InputState m_state;
    std::string m_buffer;
    QString m_note;
    int m_fd = -1;
    bool m_active = false;
    bool m_hasState = false;
};

#endif // DS4_UI_INPUTMONITOR_H
