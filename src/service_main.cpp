// SPDX-License-Identifier: GPL-3.0-only

#include "drivebeaconservice.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusConnection>

namespace {
constexpr auto busName = "io.github.clmates.DriveBeacon";
constexpr auto objectPath = "/io/github/clmates/DriveBeacon";
}

/** Starts the headless D-Bus owner for one DriveBeacon profile. */
int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    application.setOrganizationDomain(QStringLiteral("io.github.clmates"));
    application.setOrganizationName(QStringLiteral("clmates"));
    // Keep QSettings aligned with the tray so both processes read the same
    // profiles/active entry and Graph baselines.
    application.setApplicationName(QStringLiteral("drivebeacon"));

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("profile"),
                      QStringLiteral("Synchronization profile to serve."),
                      QStringLiteral("name")});
    parser.process(application);

    DriveBeaconService service(parser.value(QStringLiteral("profile")));
    auto connection = QDBusConnection::sessionBus();
    if (!connection.isConnected()
        || !connection.registerService(QString::fromLatin1(busName))
        || !connection.registerObject(QString::fromLatin1(objectPath), &service,
                                      QDBusConnection::ExportAllContents)) {
        return 1;
    }
    return application.exec();
}
