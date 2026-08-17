#ifndef DS4_UI_TRAYICON_H
#define DS4_UI_TRAYICON_H

#include <QActionGroup>
#include <QList>
#include <QMenu>
#include <QObject>
#include <QSystemTrayIcon>

class AppController;
class DaemonController;

// The applet: a tray icon whose menu switches the emulated controller type
// without opening the window, and which shows what the daemon is currently
// doing in its tooltip.
//
// Uses QtWidgets because QSystemTrayIcon and QMenu live there -- Qt Quick has
// no tray API. The window itself stays QML.
class TrayIcon : public QObject {
    Q_OBJECT

public:
    TrayIcon(DaemonController *daemon, AppController *app, QObject *parent = nullptr);
    ~TrayIcon() override;

    // True if a StatusNotifierItem host / tray actually accepted the icon.
    static bool isAvailable();

private slots:
    void onStatusChanged();
    void onActivated(QSystemTrayIcon::ActivationReason reason);

private:
    void buildMenu();
    QString summary() const;

    DaemonController *m_daemon;
    AppController *m_app;
    QSystemTrayIcon *m_tray;
    QMenu *m_menu;
    QAction *m_summaryAction = nullptr;
    QActionGroup *m_typeGroup = nullptr;
    QList<QAction *> m_typeActions;
};

#endif // DS4_UI_TRAYICON_H
