#include "appcontroller.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

namespace {

// QSettings keys. Kept in one place so the stored file stays stable if the
// property names ever change.
const char *const kTrayEnabledKey = "ui/trayEnabled";

// Basename of the XDG autostart entry, without the .desktop suffix. Matches
// the file the Makefile's uninstall targets clean up.
const char *const kAutostartName = "ds4-translator-applet";

} // namespace

AppController::AppController(QObject *parent) : QObject(parent) {
    QSettings settings;
    // Default false: the applet is opt-in, both here and for autostart.
    m_trayEnabled = settings.value(QLatin1String(kTrayEnabledKey), false).toBool();
    // The autostart state is not stored in QSettings -- the file on disk is
    // the truth, since the user (or a package) can add or remove it directly.
    m_autostartEnabled = QFileInfo::exists(autostartFilePath());
}

void AppController::setTrayAvailable(bool available) {
    if (m_trayAvailable == available) {
        return;
    }
    m_trayAvailable = available;
    emit trayAvailableChanged();
    emit trayActiveChanged();
}

void AppController::setTrayEnabled(bool enabled) {
    if (m_trayEnabled == enabled) {
        return;
    }
    m_trayEnabled = enabled;

    QSettings settings;
    settings.setValue(QLatin1String(kTrayEnabledKey), enabled);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        setSettingsError(tr("Could not save the tray preference to %1.").arg(settings.fileName()));
    } else {
        setSettingsError(QString());
    }

    emit trayEnabledChanged();
    emit trayActiveChanged();

    // Autostart launches the applet with --background, which only makes sense
    // with a tray icon to land in; without one the session would just open a
    // settings window at every login.
    if (!enabled) {
        setAutostartEnabled(false);
    }
}

QString AppController::autostartFilePath() {
    // GenericConfigLocation is $XDG_CONFIG_HOME (~/.config), where the XDG
    // autostart spec puts per-user entries.
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return base + QLatin1String("/autostart/") + QLatin1String(kAutostartName) +
           QLatin1String(".desktop");
}

bool AppController::writeAutostartFile(QString *error) const {
    const QString path = autostartFilePath();
    const QDir dir = QFileInfo(path).dir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        *error = tr("Could not create %1.").arg(dir.path());
        return false;
    }

    // The absolute path of the running binary rather than a bare command
    // name: the app is commonly installed in ~/.local/bin, which is not on
    // the PATH of every session's autostart launcher.
    const QString exec = QCoreApplication::applicationFilePath();
    const QString contents = QStringLiteral(
                                 "[Desktop Entry]\n"
                                 "Type=Application\n"
                                 "Name=DS4 Translator Applet\n"
                                 "Comment=Tray applet for switching the emulated controller type\n"
                                 "Exec=%1 --background\n"
                                 "Icon=input-gaming\n"
                                 "Terminal=false\n"
                                 "NoDisplay=true\n"
                                 "X-GNOME-Autostart-enabled=true\n")
                                 .arg(exec);

    // QSaveFile: a failed write leaves any existing entry untouched instead
    // of truncating it into something the session would fail to launch.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        *error = tr("Could not write %1: %2").arg(path, file.errorString());
        return false;
    }
    if (file.write(contents.toUtf8()) < 0 || !file.commit()) {
        *error = tr("Could not write %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

void AppController::setAutostartEnabled(bool enabled) {
    if (m_autostartEnabled == enabled) {
        return;
    }

    QString error;
    bool ok = false;
    if (enabled) {
        ok = writeAutostartFile(&error);
    } else {
        const QString path = autostartFilePath();
        ok = !QFileInfo::exists(path) || QFile::remove(path);
        if (!ok) {
            error = tr("Could not remove %1.").arg(path);
        }
    }

    if (!ok) {
        setSettingsError(error);
        // Notify anyway: the switch in the window optimistically moved to the
        // new position and has to snap back to what is really on disk.
        emit autostartEnabledChanged();
        return;
    }

    m_autostartEnabled = enabled;
    setSettingsError(QString());
    emit autostartEnabledChanged();
}

void AppController::setSettingsError(const QString &error) {
    if (m_settingsError == error) {
        return;
    }
    m_settingsError = error;
    emit settingsErrorChanged();
}
