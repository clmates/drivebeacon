#include "onedrivecontroller.h"

#include "activityparser.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>

OneDriveController::OneDriveController(QObject *parent)
    : QObject(parent)
    , m_syncDirectory(QDir::home().filePath(QStringLiteral("OneDrive")))
{
    connect(&m_systemdManager, &SystemdManager::stateChanged,
            this, &OneDriveController::stateChanged);
    connect(&m_systemdManager, &SystemdManager::errorMessageChanged,
            this, &OneDriveController::errorMessageChanged);
    connect(&m_journalReader, &JournalReader::errorOccurred,
            this, &OneDriveController::setJournalError);
    connect(&m_journalReader, &JournalReader::messageReceived, this,
            [this](const QString &message, const QDateTime &timestamp) {
                if (auto event = ActivityParser::parse(message, timestamp)) {
                    m_activities.prepend(std::move(*event));
                }
            });
    connect(&m_configProcess, &QProcess::finished, this,
            [this](int, QProcess::ExitStatus) {
                const QString output = QString::fromLocal8Bit(m_configProcess.readAllStandardOutput());
                static const QRegularExpression syncDirectoryExpression(
                    QStringLiteral("^Config option 'sync_dir'\\s*=\\s*(.+)$"),
                    QRegularExpression::MultilineOption);
                const auto match = syncDirectoryExpression.match(output);
                if (!match.hasMatch()) {
                    return;
                }

                QString path = match.captured(1).trimmed();
                if (path.startsWith(QLatin1String("~/"))) {
                    path = QDir::home().filePath(path.sliced(2));
                }
                if (path != m_syncDirectory) {
                    m_syncDirectory = QDir::cleanPath(path);
                    Q_EMIT syncDirectoryChanged();
                }
            });

    loadConfiguration();
    m_journalReader.start();
}

ActivityModel *OneDriveController::activities()
{
    return &m_activities;
}

QString OneDriveController::activeState() const
{
    return m_systemdManager.activeState();
}

QString OneDriveController::subState() const
{
    return m_systemdManager.subState();
}

QString OneDriveController::statusText() const
{
    if (activeState() == QLatin1String("active")) {
        return tr("OneDrive is running");
    }
    if (activeState() == QLatin1String("activating")) {
        return tr("OneDrive is starting");
    }
    if (activeState() == QLatin1String("deactivating")) {
        return tr("OneDrive is stopping");
    }
    if (activeState() == QLatin1String("failed")) {
        return tr("OneDrive has failed");
    }
    if (activeState() == QLatin1String("inactive")) {
        return tr("OneDrive is stopped");
    }
    if (activeState() == QLatin1String("not-found")) {
        return tr("OneDrive service was not found");
    }
    return tr("Checking OneDrive status");
}

QString OneDriveController::errorMessage() const
{
    return !m_journalError.isEmpty() ? m_journalError : m_systemdManager.errorMessage();
}

QString OneDriveController::syncDirectory() const
{
    return m_syncDirectory;
}

void OneDriveController::startService()
{
    m_systemdManager.startService();
}

void OneDriveController::stopService()
{
    m_systemdManager.stopService();
}

void OneDriveController::restartService()
{
    m_systemdManager.restartService();
}

void OneDriveController::openActivityPath(const QString &relativePath) const
{
    const QString syncRoot = QDir::cleanPath(QFileInfo(m_syncDirectory).absoluteFilePath());
    const QString targetPath = QDir::cleanPath(QDir(syncRoot).filePath(relativePath));
    if (targetPath != syncRoot && !targetPath.startsWith(syncRoot + QDir::separator())) {
        return;
    }

    QFileInfo target(targetPath);
    QFileInfo location = target.exists() && target.isDir() ? target : QFileInfo(target.absolutePath());
    while (!location.exists() && location.absoluteFilePath() != QDir::rootPath()) {
        location = QFileInfo(location.absolutePath());
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(location.absoluteFilePath()));
}

void OneDriveController::clearError()
{
    setJournalError({});
}

void OneDriveController::loadConfiguration()
{
    m_configProcess.start(QStringLiteral("onedrive"), {QStringLiteral("--display-config")});
}

void OneDriveController::setJournalError(const QString &message)
{
    if (message == m_journalError) {
        return;
    }
    m_journalError = message;
    Q_EMIT errorMessageChanged();
}
