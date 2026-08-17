#include "appcontroller.h"

AppController::AppController(QObject *parent) : QObject(parent) {}

void AppController::setTrayAvailable(bool available) {
    if (m_trayAvailable == available) {
        return;
    }
    m_trayAvailable = available;
    emit trayAvailableChanged();
}
