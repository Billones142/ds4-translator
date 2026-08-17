#include "trayicon.h"

#include <QApplication>
#include <QIcon>
#include <QPainter>
#include <QPixmap>

#include "appcontroller.h"
#include "daemoncontroller.h"
#include "ipc-client.h"

namespace {

// Themed first, so the icon matches the rest of the desktop; the drawn
// fallback exists because icon themes are not guaranteed and a tray icon that
// resolves to nothing is invisible rather than merely ugly.
QIcon appletIcon() {
    QIcon themed = QIcon::fromTheme(QStringLiteral("input-gaming"));
    if (!themed.isNull()) {
        return themed;
    }

    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0x3d, 0x6e, 0xb4));
    painter.drawRoundedRect(6, 18, 52, 28, 12, 12);
    painter.setBrush(Qt::white);
    painter.drawEllipse(QPoint(20, 32), 5, 5);
    painter.drawEllipse(QPoint(44, 32), 5, 5);
    painter.end();
    return QIcon(pixmap);
}

} // namespace

bool TrayIcon::isAvailable() {
    return QSystemTrayIcon::isSystemTrayAvailable();
}

TrayIcon::TrayIcon(DaemonController *daemon, AppController *app, QObject *parent)
    : QObject(parent), m_daemon(daemon), m_app(app), m_tray(new QSystemTrayIcon(this)),
      m_menu(new QMenu()) {
    buildMenu();

    m_tray->setIcon(appletIcon());
    m_tray->setContextMenu(m_menu);
    connect(m_tray, &QSystemTrayIcon::activated, this, &TrayIcon::onActivated);
    connect(m_daemon, &DaemonController::statusChanged, this, &TrayIcon::onStatusChanged);

    onStatusChanged();
    m_tray->show();
}

// QMenu is a widget, so it cannot be parented to this QObject; it is owned
// outright instead.
TrayIcon::~TrayIcon() {
    delete m_menu;
}

void TrayIcon::buildMenu() {
    m_summaryAction = m_menu->addAction(QString());
    m_summaryAction->setEnabled(false);

    m_menu->addSeparator();

    QAction *header = m_menu->addAction(tr("Emulated controller"));
    header->setEnabled(false);

    // Same values, in the same order, that the window and ds4-ctl offer.
    m_typeGroup = new QActionGroup(this);
    m_typeGroup->setExclusive(true);
    for (const std::string &value : ds4ipc::type_values()) {
        const QString config = QString::fromStdString(value);
        QAction *action = m_menu->addAction(
            QString::fromStdString(ds4ipc::type_config_to_display(value)));
        action->setCheckable(true);
        action->setData(config);
        m_typeGroup->addAction(action);
        m_typeActions.append(action);
        connect(action, &QAction::triggered, this, [this, config]() {
            m_daemon->setType(config);
        });
    }

    m_menu->addSeparator();
    connect(m_menu->addAction(tr("Settings…")), &QAction::triggered, this,
            [this]() { m_app->requestShowWindow(); });
    connect(m_menu->addAction(tr("Quit")), &QAction::triggered, qApp, &QApplication::quit);
}

QString TrayIcon::summary() const {
    if (!m_daemon->online()) {
        return tr("Daemon unreachable");
    }
    const QString type =
        QString::fromStdString(ds4ipc::type_config_to_display(m_daemon->emulationType().toStdString()));
    const QString physical = m_daemon->physicalController();
    if (physical.isEmpty() || physical == QLatin1String("None")) {
        return tr("%1 — no controller connected").arg(type);
    }
    return tr("%1 — %2").arg(type, m_daemon->connectionType());
}

void TrayIcon::onStatusChanged() {
    const QString text = summary();
    m_summaryAction->setText(text);
    m_tray->setToolTip(tr("DS4 Translator: %1").arg(text));

    for (QAction *action : m_typeActions) {
        // Checking an action programmatically does not emit triggered(), so
        // this cannot loop back into a set-type command.
        action->setChecked(action->data().toString() == m_daemon->emulationType());
        action->setEnabled(m_daemon->online() && !m_daemon->busy());
    }
}

void TrayIcon::onActivated(QSystemTrayIcon::ActivationReason reason) {
    // Trigger is a left click; Context (right click) is the menu's own job.
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
        m_app->requestShowWindow();
    }
}
