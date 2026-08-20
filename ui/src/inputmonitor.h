#ifndef DS4_UI_INPUTMONITOR_H
#define DS4_UI_INPUTMONITOR_H

#include <QObject>
#include <QQmlEngine>
#include <QSocketNotifier>
#include <QString>
#include <QTimer>

#include <string>

#include "ipc-client.h"

// Live view of what the daemon currently sees, on either of its two sources:
// the emulated controller it presents to games, or the physical one it reads.
//
// This is the `test all` command, the one command whose connection stays
// open: the daemon promotes the socket into its broadcast list and pushes a
// STATE line per source about 30 times a second, so unlike the settings
// commands there is no request/response pairing here and no IpcClient queue
// involved. Both sources are tracked at all times and `source` only decides
// which one the properties below report, so switching is instant.
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
    // Which device the properties below report: Virtual (the emulated
    // controller games see) or Physical (the controller the daemon reads).
    Q_PROPERTY(Source source READ source WRITE setSource NOTIFY sourceChanged)
    // True once a STATE line arrived for the selected source; false while the
    // daemon is only sending a NOTE for it (no controller, no virtual device
    // yet, emulation off).
    Q_PROPERTY(bool hasState READ hasState NOTIFY inputChanged)
    // Why the selected source has nothing to show, in the daemon's words, or
    // the connection error.
    Q_PROPERTY(QString note READ note NOTIFY noteChanged)
    // Per-source availability, so a source picker can show which ones are
    // actually reporting before the user switches to them.
    Q_PROPERTY(bool virtualLive READ virtualLive NOTIFY inputChanged)
    Q_PROPERTY(bool physicalLive READ physicalLive NOTIFY inputChanged)

    // What the emulated device is currently asking the light bar and motors
    // to do, so a view can follow a game moving them. False until the daemon
    // has said (older ones never do).
    Q_PROPERTY(bool hasOutput READ hasOutput NOTIFY outputChanged)
    Q_PROPERTY(int ledRed READ ledRed NOTIFY outputChanged)
    Q_PROPERTY(int ledGreen READ ledGreen NOTIFY outputChanged)
    Q_PROPERTY(int ledBlue READ ledBlue NOTIFY outputChanged)
    Q_PROPERTY(int rumbleLeft READ rumbleLeft NOTIFY outputChanged)
    Q_PROPERTY(int rumbleRight READ rumbleRight NOTIFY outputChanged)

    // Report timing. False against a daemon that does not measure it.
    Q_PROPERTY(bool timingSupported READ timingSupported NOTIFY timingChanged)
    // True while measure() is collecting windows.
    Q_PROPERTY(bool measuring READ measuring NOTIFY timingChanged)
    // True once a measurement finished, so a view knows to show the result.
    Q_PROPERTY(bool measured READ measured NOTIFY timingChanged)
    // Physical reports per second over the measured span.
    Q_PROPERTY(double reportRate READ reportRate NOTIFY timingChanged)
    // Gap between physical reports, in milliseconds.
    Q_PROPERTY(double intervalMeanMs READ intervalMeanMs NOTIFY timingChanged)
    Q_PROPERTY(double intervalMinMs READ intervalMinMs NOTIFY timingChanged)
    Q_PROPERTY(double intervalMaxMs READ intervalMaxMs NOTIFY timingChanged)
    // What the daemon adds between reading a physical report and having
    // written the emulated one, in milliseconds.
    Q_PROPERTY(double latencyMeanMs READ latencyMeanMs NOTIFY timingChanged)
    Q_PROPERTY(double latencyMaxMs READ latencyMaxMs NOTIFY timingChanged)

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

    // False when the daemon is older than the motion (or the touch) fields of
    // the STATE line, so those values are zeros and not readings.
    Q_PROPERTY(bool hasMotion READ hasMotion NOTIFY inputChanged)
    Q_PROPERTY(bool hasTouch READ hasTouch NOTIFY inputChanged)

    // The touchpad tracks two fingers at once. Each slot reports whether it
    // is being touched and where, in the pad's own coordinates (0..1919 by
    // 0..942, origin top-left); an inactive slot keeps its last position.
    Q_PROPERTY(bool touch1Active READ touch1Active NOTIFY inputChanged)
    Q_PROPERTY(int touch1X READ touch1X NOTIFY inputChanged)
    Q_PROPERTY(int touch1Y READ touch1Y NOTIFY inputChanged)
    Q_PROPERTY(bool touch2Active READ touch2Active NOTIFY inputChanged)
    Q_PROPERTY(int touch2X READ touch2X NOTIFY inputChanged)
    Q_PROPERTY(int touch2Y READ touch2Y NOTIFY inputChanged)
    // Pad resolution, so a view can map a contact onto its own drawing
    // without repeating the numbers.
    Q_PROPERTY(int touchWidth READ touchWidth CONSTANT)
    Q_PROPERTY(int touchHeight READ touchHeight CONSTANT)
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

    // The daemon's two streams. The values are indices into the per-source
    // arrays below.
    enum Source {
        Virtual = 0,
        Physical = 1,
    };
    Q_ENUM(Source)

    bool isActive() const { return m_active; }
    void setActive(bool active);

    bool isConnected() const { return m_fd >= 0; }
    Source source() const { return m_source; }
    void setSource(Source source);
    bool hasState() const { return m_hasState[m_source]; }
    QString note() const;
    bool virtualLive() const { return m_hasState[Virtual]; }
    bool physicalLive() const { return m_hasState[Physical]; }

    int leftX() const { return selected().lx; }
    int leftY() const { return selected().ly; }
    int rightX() const { return selected().rx; }
    int rightY() const { return selected().ry; }
    int l2() const { return selected().l2; }
    int r2() const { return selected().r2; }
    bool hasMotion() const { return selected().has_motion; }
    bool hasTouch() const { return selected().has_touch; }
    bool touch1Active() const { return selected().touch[0].active; }
    int touch1X() const { return selected().touch[0].x; }
    int touch1Y() const { return selected().touch[0].y; }
    bool touch2Active() const { return selected().touch[1].active; }
    int touch2X() const { return selected().touch[1].x; }
    int touch2Y() const { return selected().touch[1].y; }
    static int touchWidth() { return ds4ipc::kTouchWidth; }
    static int touchHeight() { return ds4ipc::kTouchHeight; }
    int buttons() const { return selected().buttons; }
    int dpad() const { return selected().dpad; }
    int gyroPitch() const { return selected().gyro[0]; }
    int gyroYaw() const { return selected().gyro[1]; }
    int gyroRoll() const { return selected().gyro[2]; }
    int accelX() const { return selected().accel[0]; }
    int accelY() const { return selected().accel[1]; }
    int accelZ() const { return selected().accel[2]; }

    bool hasOutput() const { return m_hasOutput; }
    int ledRed() const { return m_output.red; }
    int ledGreen() const { return m_output.green; }
    int ledBlue() const { return m_output.blue; }
    int rumbleLeft() const { return m_output.rumble_left; }
    int rumbleRight() const { return m_output.rumble_right; }

    bool timingSupported() const { return m_timingSupported; }
    bool measuring() const { return m_measureWindows > 0; }
    bool measured() const { return m_measured; }
    double reportRate() const;
    double intervalMeanMs() const { return m_result.interval_mean_us / 1000.0; }
    double intervalMinMs() const { return m_result.interval_min_us / 1000.0; }
    double intervalMaxMs() const { return m_result.interval_max_us / 1000.0; }
    double latencyMeanMs() const { return m_result.latency_mean_us / 1000.0; }
    double latencyMaxMs() const { return m_result.latency_max_us / 1000.0; }

    // Starts a measurement: the daemon reports one window per second, and
    // this averages a few of them so a single unlucky window (a scheduling
    // hiccup, a controller that just woke up) does not stand for the whole
    // connection.
    Q_INVOKABLE void measure();

    // True when `bit` (a Button value) is currently held.
    Q_INVOKABLE bool isPressed(int bit) const { return (selected().buttons & bit) != 0; }
    // "Up", "Down-Left", "Neutral", ... for the current hat value.
    Q_INVOKABLE QString dpadName() const;

signals:
    void activeChanged();
    void connectedChanged();
    void sourceChanged();
    void noteChanged();
    void timingChanged();
    void outputChanged();
    // One signal for the whole input snapshot: every field of a STATE line
    // changes together, ~30 times a second, so splitting it into a signal per
    // property would only multiply the work with no gain.
    void inputChanged();

private slots:
    void onReadable();
    void tryConnect();

private:
    static constexpr int kSourceCount = 2;

    const ds4ipc::InputState &selected() const { return m_states[m_source]; }
    void disconnectStream();
    void handleLine(const std::string &line);
    // Index of a STATE/NOTE line's source word, or -1 when it names none.
    static int sourceIndex(const std::string &source);
    void setNote(int source, const QString &note);
    void applyTiming(const ds4ipc::TimingStats &window);
    void setConnectionNote(const QString &note);
    void clearStates();

    QTimer *m_retry;
    QSocketNotifier *m_notifier = nullptr;
    ds4ipc::InputState m_states[kSourceCount];
    std::string m_buffer;
    // Per-source reason for having nothing to show, straight from the daemon.
    QString m_notes[kSourceCount];
    // Set instead of the per-source notes while the stream itself is down, so
    // an unreachable daemon is not reported as an idle controller.
    QString m_connectionNote;
    Source m_source = Virtual;
    // Cleared when the daemon does not know "test all" (one older than the
    // two-source stream): the plain "test" command is used instead, and only
    // the source that daemon considers active reports anything.
    bool m_allSources = true;
    int m_fd = -1;
    bool m_active = false;
    bool m_hasState[kSourceCount] = {false, false};

    // Measurement state. m_measureWindows counts down the daemon windows
    // still to collect; m_accumulated sums them, m_result is the finished
    // measurement the view displays.
    int m_measureWindows = 0;
    ds4ipc::TimingStats m_accumulated;
    ds4ipc::TimingStats m_result;
    ds4ipc::OutputState m_output;
    bool m_hasOutput = false;
    bool m_timingSupported = false;
    bool m_measured = false;
};

#endif // DS4_UI_INPUTMONITOR_H
