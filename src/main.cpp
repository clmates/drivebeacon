// SPDX-License-Identifier: GPL-3.0-only

#include "onedrivecontroller.h"

#include <KAboutData>
#include <KAboutApplicationDialog>
#include <KLocalizedString>
#include <KLocalizedQmlContext>
#include <KStatusNotifierItem>

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
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

    OneDriveController controller;
    QQmlApplicationEngine engine;
    KLocalization::setupLocalizedContext(&engine);
    engine.rootContext()->setContextProperty(QStringLiteral("controller"), &controller);
    engine.rootContext()->setContextProperty(
        QStringLiteral("applicationAboutData"), QVariant::fromValue(aboutData));
    engine.loadFromModule(QStringLiteral("io.github.clmates.drivebeacon"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    KStatusNotifierItem tray(QStringLiteral("drivebeacon"));
    tray.setCategory(KStatusNotifierItem::SystemServices);
    tray.setIconByName(QStringLiteral("folder-cloud"));
    tray.setTitle(i18n("DriveBeacon"));

    QMenu menu;
    QAction showAction(i18n("Show status"), &menu);
    QAction startAction(i18n("Start synchronization"), &menu);
    QAction stopAction(i18n("Stop synchronization"), &menu);
    QAction restartAction(i18n("Restart synchronization"), &menu);
    QAction aboutAction(i18n("About DriveBeacon"), &menu);
    QAction quitAction(i18n("Quit"), &menu);
    menu.addAction(&showAction);
    menu.addSeparator();
    menu.addAction(&startAction);
    menu.addAction(&stopAction);
    menu.addAction(&restartAction);
    menu.addSeparator();
    menu.addAction(&aboutAction);
    menu.addAction(&quitAction);
    tray.setContextMenu(&menu);

    const auto showWindow = [window] {
        if (!window) {
            return;
        }
        window->show();
        window->raise();
        window->requestActivate();
    };
    QObject::connect(&showAction, &QAction::triggered, &application, showWindow);
    QObject::connect(&tray, &KStatusNotifierItem::activateRequested, &application,
                     [showWindow](bool, const QPoint &) { showWindow(); });
    QObject::connect(&startAction, &QAction::triggered,
                     &controller, &OneDriveController::startService);
    QObject::connect(&stopAction, &QAction::triggered,
                     &controller, &OneDriveController::stopService);
    QObject::connect(&restartAction, &QAction::triggered,
                     &controller, &OneDriveController::restartService);
    KAboutApplicationDialog aboutDialog(aboutData);
    QObject::connect(&aboutAction, &QAction::triggered, &aboutDialog, [&aboutDialog] {
        aboutDialog.show();
        aboutDialog.raise();
        aboutDialog.activateWindow();
    });
    QObject::connect(&quitAction, &QAction::triggered,
                     &application, &QApplication::quit);

    const auto updateTray = [&] {
        const bool running = controller.activeState() == QLatin1String("active");
        startAction.setEnabled(!running);
        stopAction.setEnabled(running);
        restartAction.setEnabled(running);
        tray.setStatus(controller.activeState() == QLatin1String("failed")
                           ? KStatusNotifierItem::NeedsAttention
                           : running ? KStatusNotifierItem::Active
                                     : KStatusNotifierItem::Passive);
        tray.setToolTip(QStringLiteral("folder-cloud"), i18n("DriveBeacon"),
                        controller.statusText());
    };
    QObject::connect(&controller, &OneDriveController::stateChanged, &application, updateTray);
    updateTray();

    return application.exec();
}
