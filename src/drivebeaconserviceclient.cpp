// SPDX-License-Identifier: GPL-3.0-only

#include "drivebeaconserviceclient.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusReply>
#include <QDBusVariant>
#include <QTimer>

namespace {
const QString serviceName = QStringLiteral("io.github.clmates.DriveBeacon");
const QString objectPath = QStringLiteral("/io/github/clmates/DriveBeacon");
const QString interfaceName = QStringLiteral("io.github.clmates.DriveBeacon1");
}

DriveBeaconServiceClient::DriveBeaconServiceClient(QObject *parent)
    : QObject(parent)
{
    auto connection = QDBusConnection::sessionBus();
    connection.connect(serviceName, objectPath, interfaceName, QStringLiteral("statusChanged"),
                       this, SLOT(onRemoteStatusChanged()));
    auto *watcher = new QDBusServiceWatcher(
        serviceName, connection,
        QDBusServiceWatcher::WatchForRegistration
            | QDBusServiceWatcher::WatchForUnregistration,
        this);
    connect(watcher, &QDBusServiceWatcher::serviceRegistered,
            this, &DriveBeaconServiceClient::refresh);
    connect(watcher, &QDBusServiceWatcher::serviceUnregistered,
            this, &DriveBeaconServiceClient::refresh);
    QTimer::singleShot(0, this, &DriveBeaconServiceClient::refresh);
}

bool DriveBeaconServiceClient::available() const
{
    return m_available;
}

QString DriveBeaconServiceClient::syncStatus() const
{
    return m_syncStatus;
}

int DriveBeaconServiceClient::syncProgress() const
{
    return m_syncProgress;
}

bool DriveBeaconServiceClient::graphAuthenticated() const
{
    return m_graphAuthenticated;
}

QString DriveBeaconServiceClient::errorMessage() const
{
    return m_errorMessage;
}

bool DriveBeaconServiceClient::graphSyncEnabled() const
{
    return m_graphSyncEnabled;
}

bool DriveBeaconServiceClient::globalSyncEnabled() const
{
    return m_globalSyncEnabled;
}

void DriveBeaconServiceClient::refresh()
{
    const auto connection = QDBusConnection::sessionBus();
    const bool present = connection.interface()->isServiceRegistered(serviceName);
    if (present != m_available) {
        m_available = present;
        Q_EMIT availabilityChanged();
    }
    if (!present) {
        m_syncStatus = QStringLiteral("Service unavailable");
        m_syncProgress = 0;
        m_graphAuthenticated = false;
        m_errorMessage.clear();
        m_graphSyncEnabled = false;
        m_globalSyncEnabled = true;
        Q_EMIT statusChanged();
        return;
    }

    QDBusInterface properties(serviceName, objectPath,
                              QStringLiteral("org.freedesktop.DBus.Properties"), connection);
    auto *watcher = new QDBusPendingCallWatcher(
        properties.asyncCall(QStringLiteral("GetAll"), interfaceName), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        const QDBusPendingReply<QVariantMap> reply = *watcher;
        if (reply.isError()) {
            Q_EMIT errorOccurred(reply.error().message());
        } else {
            applyProperties(reply.value());
        }
        watcher->deleteLater();
    });
}

void DriveBeaconServiceClient::synchronizeGraph()
{
    call(QStringLiteral("synchronizeGraph"));
}

void DriveBeaconServiceClient::refreshGraphFolders()
{
    call(QStringLiteral("refreshGraphFolders"));
}

void DriveBeaconServiceClient::setProfileSyncEnabled(const QString &profileName, bool enabled)
{
    call(QStringLiteral("setProfileSyncEnabled"), {profileName, enabled});
}

void DriveBeaconServiceClient::setGlobalSyncEnabled(bool enabled)
{
    call(QStringLiteral("setGlobalSyncEnabled"), {enabled});
}

void DriveBeaconServiceClient::onRemoteStatusChanged()
{
    refresh();
}

void DriveBeaconServiceClient::call(const QString &method, const QVariantList &arguments)
{
    if (!m_available) {
        Q_EMIT errorOccurred(QStringLiteral("DriveBeacon service is unavailable."));
        return;
    }
    QDBusInterface service(serviceName, objectPath, interfaceName,
                           QDBusConnection::sessionBus());
    auto *watcher = new QDBusPendingCallWatcher(
        service.asyncCallWithArgumentList(method, arguments), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        const QDBusPendingReply<> reply = *watcher;
        if (reply.isError()) {
            Q_EMIT errorOccurred(reply.error().message());
        }
        watcher->deleteLater();
    });
}

void DriveBeaconServiceClient::applyProperties(const QVariantMap &properties)
{
    const QString status = properties.value(QStringLiteral("syncStatus")).toString();
    const int progress = properties.value(QStringLiteral("syncProgress")).toInt();
    const bool authenticated = properties.value(QStringLiteral("graphAuthenticated")).toBool();
    const QString error = properties.value(QStringLiteral("errorMessage")).toString();
    const bool syncEnabled = properties.value(QStringLiteral("graphSyncEnabled")).toBool();
    const bool globalSyncEnabled = properties.value(QStringLiteral("globalSyncEnabled")).toBool();
    if (status == m_syncStatus && progress == m_syncProgress
        && authenticated == m_graphAuthenticated && error == m_errorMessage
        && syncEnabled == m_graphSyncEnabled
        && globalSyncEnabled == m_globalSyncEnabled) {
        return;
    }
    m_syncStatus = status;
    m_syncProgress = progress;
    m_graphAuthenticated = authenticated;
    m_errorMessage = error;
    m_graphSyncEnabled = syncEnabled;
    m_globalSyncEnabled = globalSyncEnabled;
    Q_EMIT statusChanged();
}
