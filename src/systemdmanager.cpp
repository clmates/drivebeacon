#include "systemdmanager.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDBusVariant>

namespace {
constexpr auto serviceName = "org.freedesktop.systemd1";
constexpr auto managerPath = "/org/freedesktop/systemd1";
constexpr auto managerInterface = "org.freedesktop.systemd1.Manager";
constexpr auto propertiesInterface = "org.freedesktop.DBus.Properties";
constexpr auto unitInterface = "org.freedesktop.systemd1.Unit";
constexpr auto unitName = "onedrive.service";
}

SystemdManager::SystemdManager(QObject *parent)
    : QObject(parent)
{
    m_refreshTimer.setInterval(3000);
    connect(&m_refreshTimer, &QTimer::timeout, this, &SystemdManager::refresh);
    m_refreshTimer.start();
    QTimer::singleShot(0, this, &SystemdManager::refresh);
}

QString SystemdManager::activeState() const
{
    return m_activeState;
}

QString SystemdManager::subState() const
{
    return m_subState;
}

QString SystemdManager::errorMessage() const
{
    return m_errorMessage;
}

void SystemdManager::refresh()
{
    auto connection = QDBusConnection::sessionBus();
    if (!connection.isConnected()) {
        setErrorMessage(tr("Cannot connect to the user D-Bus session."));
        return;
    }

    QDBusInterface manager(QLatin1String(serviceName), QLatin1String(managerPath),
                           QLatin1String(managerInterface), connection);
    const QDBusReply<QDBusObjectPath> unitReply = manager.call(QStringLiteral("GetUnit"),
                                                                QLatin1String(unitName));
    if (!unitReply.isValid()) {
        if (m_activeState != QLatin1String("not-found")) {
            m_activeState = QStringLiteral("not-found");
            m_subState.clear();
            Q_EMIT stateChanged();
        }
        setErrorMessage(unitReply.error().message());
        return;
    }

    QDBusInterface properties(QLatin1String(serviceName), unitReply.value().path(),
                              QLatin1String(propertiesInterface), connection);
    const QDBusReply<QDBusVariant> activeReply = properties.call(
        QStringLiteral("Get"), QLatin1String(unitInterface), QStringLiteral("ActiveState"));
    const QDBusReply<QDBusVariant> subReply = properties.call(
        QStringLiteral("Get"), QLatin1String(unitInterface), QStringLiteral("SubState"));
    if (!activeReply.isValid() || !subReply.isValid()) {
        setErrorMessage(activeReply.isValid() ? subReply.error().message()
                                              : activeReply.error().message());
        return;
    }

    const QString activeState = activeReply.value().variant().toString();
    const QString subState = subReply.value().variant().toString();
    if (activeState != m_activeState || subState != m_subState) {
        m_activeState = activeState;
        m_subState = subState;
        Q_EMIT stateChanged();
    }
    setErrorMessage({});
}

void SystemdManager::startService()
{
    callManager(QStringLiteral("StartUnit"));
}

void SystemdManager::stopService()
{
    callManager(QStringLiteral("StopUnit"));
}

void SystemdManager::restartService()
{
    callManager(QStringLiteral("RestartUnit"));
}

void SystemdManager::callManager(const QString &method)
{
    auto connection = QDBusConnection::sessionBus();
    QDBusInterface manager(QLatin1String(serviceName), QLatin1String(managerPath),
                           QLatin1String(managerInterface), connection);
    auto *watcher = new QDBusPendingCallWatcher(
        manager.asyncCall(method, QLatin1String(unitName), QStringLiteral("replace")), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        const QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        if (reply.isError()) {
            setErrorMessage(reply.error().message());
        }
        watcher->deleteLater();
        QTimer::singleShot(250, this, &SystemdManager::refresh);
    });
}

void SystemdManager::setErrorMessage(const QString &message)
{
    if (message == m_errorMessage) {
        return;
    }
    m_errorMessage = message;
    Q_EMIT errorMessageChanged();
}
