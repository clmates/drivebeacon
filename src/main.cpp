// SPDX-License-Identifier: GPL-3.0-only

#include "onedrivecontroller.h"
#include "profiledialog.h"
#include "drivebeaconserviceclient.h"

#include <KAboutData>
#include <KAboutApplicationDialog>
#include <KLocalizedString>
#include <KStatusNotifierItem>

#include <QAction>
#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QLocale>
#include <QMenu>
#include <QInputDialog>
#include <QProcess>

namespace {
/** Formats byte counts for the tray without exposing provider-specific units. */
QString formatBytes(qint64 bytes)
{
    return bytes < 0 ? i18n("Unavailable")
                     : QLocale().formattedDataSize(bytes, 2, QLocale::DataSizeTraditionalFormat);
}
}

/** Creates the application and exposes status and activity through a native tray menu. */
int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    application.setOrganizationDomain(QStringLiteral("io.github.clmates"));
    application.setOrganizationName(QStringLiteral("clmates"));
    application.setApplicationName(QStringLiteral("drivebeacon"));
    KLocalizedString::setApplicationDomain("drivebeacon");

    KAboutData aboutData(
        QStringLiteral("drivebeacon"),
        i18n("DriveBeacon"),
        QStringLiteral(DRIVEBEACON_VERSION),
        i18n("Monitor and control OneDrive synchronization"),
        KAboutLicense::GPL_V3);
    aboutData.setDesktopFileName(QStringLiteral("io.github.clmates.drivebeacon"));
    aboutData.setHomepage(QStringLiteral("https://github.com/clmates/drivebeacon"));
    aboutData.setBugAddress("https://github.com/clmates/drivebeacon/issues");
    aboutData.setCopyrightStatement(i18n("Copyright 2026 DriveBeacon contributors"));
    aboutData.setOtherText(i18n(
        "DriveBeacon is an independent open-source project. It is not affiliated "
        "with or endorsed by Microsoft Corporation or KDE e.V."));
    aboutData.addAuthor(QStringLiteral("clmates"), i18n("Development"));
    KAboutData::setApplicationData(aboutData);

    QCommandLineParser commandLine;
    commandLine.setApplicationDescription(i18n("Monitor and control OneDrive synchronization"));
    commandLine.addHelpOption();
    commandLine.addOption({QStringLiteral("profile"),
                           i18n("Use an isolated synchronization profile."),
                           QStringLiteral("name")});
    commandLine.addOption({QStringLiteral("backend"),
                           i18n("Override the profile backend (abraunegg-journal or graph)."),
                           QStringLiteral("backend")});
    commandLine.addOption({QStringLiteral("local-directory"),
                           i18n("Override the profile local directory."),
                           QStringLiteral("path")});
    commandLine.process(application);

    OneDriveController controller(commandLine.value(QStringLiteral("profile")),
                                  commandLine.value(QStringLiteral("backend")),
                                  commandLine.value(QStringLiteral("local-directory")),
                                  false);
    DriveBeaconServiceClient serviceClient;
    SystemdManager graphServiceManager(QStringLiteral("drivebeacon-service.service"));
    const QString trayId = QStringLiteral("drivebeacon-%1").arg(controller.profileName());
    KStatusNotifierItem tray(trayId);
    tray.setCategory(KStatusNotifierItem::SystemServices);
    tray.setIconByName(QStringLiteral("folder-cloud"));
    tray.setTitle(i18n("DriveBeacon"));
    // KStatusNotifierItem contributes the localized standard Quit action;
    // keep it as the single application-exit entry in the tray menu.
    tray.setStandardActionsEnabled(true);

    QAction *statusAction = new QAction(&application);
    statusAction->setEnabled(false);
    QAction *directoryAction = new QAction(&application);
    directoryAction->setEnabled(false);
    QAction *remoteQuotaAction = new QAction(&application);
    remoteQuotaAction->setEnabled(false);
    QAction *graphSyncAction = new QAction(&application);
    QAction *activityHeader = new QAction(i18n("Recent activity"), &application);
    activityHeader->setEnabled(false);
    QAction *emptyActivityAction = new QAction(
        i18n("No recent file activity was found in the journal."), &application);
    emptyActivityAction->setEnabled(false);
    QAction *clearActivityAction = new QAction(i18n("Clear activity"), &application);
    QAction *startAction = new QAction(i18n("Start synchronization"), &application);
    QAction *stopAction = new QAction(i18n("Stop synchronization"), &application);
    QAction *restartAction = new QAction(i18n("Restart synchronization"), &application);
    QAction *configurationAction = new QAction(i18n("Configure profiles…"), &application);
    QAction *aboutAction = new QAction(i18n("About DriveBeacon"), &application);

    auto *popupMenu = new QMenu;
    popupMenu->addAction(statusAction);
    popupMenu->addAction(directoryAction);
    popupMenu->addAction(remoteQuotaAction);
    popupMenu->addAction(graphSyncAction);
    popupMenu->addSeparator();
    popupMenu->addAction(activityHeader);
    popupMenu->addAction(emptyActivityAction);
    popupMenu->addAction(clearActivityAction);
    popupMenu->addSeparator();

    auto addServiceActions = [=](QMenu *menu) {
        menu->addAction(startAction);
        menu->addAction(stopAction);
        menu->addAction(restartAction);
        menu->addSeparator();
        menu->addAction(configurationAction);
        menu->addAction(aboutAction);
    };

    addServiceActions(popupMenu);
    // The tray host owns the popup surface and positions it next to the tray on Wayland.
    tray.setContextMenu(popupMenu);
    tray.setIsMenu(true);
    QObject::connect(startAction, &QAction::triggered, &application, [&] {
        if (controller.usesGraphService()) {
            graphServiceManager.startService();
        } else {
            controller.startService();
        }
    });
    QObject::connect(stopAction, &QAction::triggered, &application, [&] {
        if (controller.usesGraphService()) {
            graphServiceManager.stopService();
        } else {
            controller.stopService();
        }
    });
    QObject::connect(restartAction, &QAction::triggered, &application, [&] {
        if (controller.usesGraphService()) {
            graphServiceManager.restartService();
        } else {
            controller.restartService();
        }
    });
    QObject::connect(graphSyncAction, &QAction::triggered, &application, [&] {
        if (serviceClient.available()) {
            serviceClient.synchronizeGraph();
        } else {
            controller.synchronizeGraph();
        }
    });
    QObject::connect(clearActivityAction, &QAction::triggered,
                     controller.activities(), &ActivityModel::clear);
    QObject::connect(configurationAction, &QAction::triggered, &application, [&] {
        auto *dialog = new ProfileDialog(controller.profileStore(), &controller, nullptr);
        QObject::connect(dialog, &ProfileDialog::useProfileRequested,
                         &application, [dialog, &application](const QString &profileName) {
                             const QString executable = QCoreApplication::applicationFilePath();
                             QProcess::startDetached(executable, {QStringLiteral("--profile"), profileName});
                             dialog->close();
                             dialog->deleteLater();
                             application.quit();
                         });
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
    });
    QObject::connect(&controller, &OneDriveController::legacyProfileDetected,
                     &application, [&controller](const QString &) {
                         bool accepted = false;
                         const QString profileName = QInputDialog::getText(
                             nullptr,
                             i18n("Import abraunegg configuration"),
                             i18n("Profile name for the existing abraunegg configuration:"),
                             QLineEdit::Normal,
                             QStringLiteral("abraunegg"),
                             &accepted);
                         if (accepted) {
                             controller.confirmLegacyMigration(profileName);
                         } else {
                             controller.dismissLegacyMigration();
                         }
                     });

    QList<QAction *> activityActions;
    for (int row = 0; row < 20; ++row) {
        auto *action = new QAction(popupMenu);
        action->setVisible(false);
        QObject::connect(action, &QAction::triggered, &controller,
                         [action, controller = &controller] {
                             controller->openActivityPath(action->data().toString());
                         });
        popupMenu->insertAction(clearActivityAction, action);
        activityActions.append(action);
    }

    const auto rebuildActivities = [&] {
        const int count = controller.activities()->rowCount();
        emptyActivityAction->setVisible(count == 0);
        for (int row = 0; row < activityActions.size(); ++row) {
            QAction *action = activityActions.at(row);
            if (row >= count) {
                action->setVisible(false);
                continue;
            }

            const QModelIndex index = controller.activities()->index(row, 0);
            const QString operation = index.data(ActivityModel::OperationRole).toString();
            const QString path = index.data(ActivityModel::PathRole).toString();
            const QString destination = index.data(ActivityModel::DestinationPathRole).toString();
            const bool completed = index.data(ActivityModel::CompletedRole).toBool();
            const QString activityPath = destination.isEmpty() ? path : destination;
            const QString state = operation == QLatin1String("download")
                ? i18n("Downloaded")
                : operation == QLatin1String("upload")
                    ? i18n("Uploaded")
                    : operation == QLatin1String("move")
                        ? i18n("Moved")
                        : completed ? i18n("Deleted") : i18n("Deleting");
            const QString label = operation == QLatin1String("graph-log")
                || operation == QLatin1String("graph-progress")
                ? index.data(ActivityModel::MessageRole).toString()
                : destination.isEmpty()
                ? QStringLiteral("%1 · %2").arg(path, state)
                : QStringLiteral("%1 → %2 · %3").arg(path, destination, state);
            action->setText(label);
            action->setData(activityPath);
            action->setVisible(true);
        }
    };
    QObject::connect(controller.activities(), &QAbstractItemModel::rowsInserted,
                     &application, rebuildActivities);
    QObject::connect(controller.activities(), &QAbstractItemModel::rowsRemoved,
                     &application, rebuildActivities);
    QObject::connect(controller.activities(), &QAbstractItemModel::dataChanged,
                     &application, rebuildActivities);
    QObject::connect(controller.activities(), &QAbstractItemModel::rowsMoved,
                     &application, rebuildActivities);
    QObject::connect(controller.activities(), &QAbstractItemModel::modelReset,
                     &application, rebuildActivities);
    rebuildActivities();

    KAboutApplicationDialog aboutDialog(aboutData);
    QObject::connect(aboutAction, &QAction::triggered, &aboutDialog, [&aboutDialog] {
        aboutDialog.show();
        aboutDialog.raise();
        aboutDialog.activateWindow();
    });
    QObject::connect(&tray, &KStatusNotifierItem::quitRequested,
                     &application, &QApplication::quit);

    // Keep tray actions and attention state synchronized with the backend properties.
    const auto updateTray = [&] {
        const bool graphService = controller.usesGraphService();
        const QString serviceState = graphService
            ? graphServiceManager.activeState() : controller.activeState();
        const bool running = serviceState == QLatin1String("active");
        statusAction->setText(i18n("Status: %1", controller.statusText()));
        directoryAction->setText(i18n("Local folder: %1", controller.syncDirectory()));
        remoteQuotaAction->setText(i18n("Remote storage: %1 used · %2 available · %3 total",
                                       formatBytes(controller.remoteQuotaUsed()),
                                       formatBytes(controller.remoteQuotaRemaining()),
                                       formatBytes(controller.remoteQuotaTotal())));
        const bool serviceControl = graphService || controller.serviceControlAvailable();
        const QString syncStatus = serviceClient.available()
            ? serviceClient.syncStatus() : controller.graphSyncStatus();
        const bool authenticated = serviceClient.available()
            ? serviceClient.graphAuthenticated() : controller.graphAuthenticated();
        graphSyncAction->setText(i18n("Graph sync: %1", syncStatus));
        graphSyncAction->setEnabled(!serviceControl && authenticated);
        startAction->setEnabled(serviceControl && !running);
        stopAction->setEnabled(serviceControl && running);
        restartAction->setEnabled(serviceControl && running);
        tray.setStatus(serviceState == QLatin1String("failed")
                           ? KStatusNotifierItem::NeedsAttention
                           : running ? KStatusNotifierItem::Active
                                     : KStatusNotifierItem::Passive);
        tray.setToolTip(QStringLiteral("folder-cloud"), i18n("DriveBeacon"),
                        controller.statusText());
    };
    QObject::connect(&controller, &OneDriveController::stateChanged, &application, updateTray);
    QObject::connect(&graphServiceManager, &SystemdManager::stateChanged,
                     &application, updateTray);
    QObject::connect(&controller, &OneDriveController::remoteQuotaChanged,
                     &application, updateTray);
    QObject::connect(&controller, &OneDriveController::profileChanged,
                     &application, updateTray);
    QObject::connect(&controller, &OneDriveController::graphAuthChanged,
                     &application, updateTray);
    QObject::connect(&controller, &OneDriveController::graphSyncChanged,
                     &application, updateTray);
    QObject::connect(&serviceClient, &DriveBeaconServiceClient::availabilityChanged,
                     &application, updateTray);
    QObject::connect(&serviceClient, &DriveBeaconServiceClient::statusChanged,
                     &application, updateTray);
    updateTray();

    return application.exec();
}
