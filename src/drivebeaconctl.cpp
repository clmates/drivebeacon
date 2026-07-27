// SPDX-License-Identifier: GPL-3.0-only

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>

namespace {
constexpr auto serviceName = "io.github.clmates.DriveBeacon";
constexpr auto objectPath = "/io/github/clmates/DriveBeacon";
constexpr auto interfaceName = "io.github.clmates.DriveBeacon1";
constexpr auto propertiesInterface = "org.freedesktop.DBus.Properties";
constexpr auto serviceUnit = "drivebeacon-service.service";

/** Prints a command failure consistently and returns the CLI error code. */
int printError(const QString &message)
{
    QTextStream(stderr) << "drivebeaconctl: " << message << Qt::endl;
    return 1;
}

/** Creates the synchronous proxy used by short-lived CLI commands. */
QDBusInterface serviceInterface()
{
    return QDBusInterface(QString::fromLatin1(serviceName),
                          QString::fromLatin1(objectPath),
                          QString::fromLatin1(interfaceName),
                          QDBusConnection::sessionBus());
}

/** Prints the service properties exposed to all non-graphical clients. */
int printStatus()
{
    QDBusInterface service = serviceInterface();
    if (!service.isValid()) {
        return printError(service.lastError().message());
    }
    QDBusInterface properties(QString::fromLatin1(serviceName),
                              QString::fromLatin1(objectPath),
                              QString::fromLatin1(propertiesInterface),
                              QDBusConnection::sessionBus());
    const QDBusReply<QVariantMap> reply = properties.call(
        QStringLiteral("GetAll"), QString::fromLatin1(interfaceName));
    if (!reply.isValid()) {
        return printError(reply.error().message());
    }
    const QVariantMap values = reply.value();
    QTextStream output(stdout);
    output << "Service: available\n"
           << "Global sync: "
           << (values.value(QStringLiteral("globalSyncEnabled")).toBool() ? "enabled" : "paused")
           << "\n";
    output << "Active profile: " << values.value(QStringLiteral("profileName")).toString()
           << "\n";
    const QStringList profiles = values.value(QStringLiteral("graphProfiles")).toStringList();
    for (const QString &profile : profiles) {
        const QDBusReply<QVariantMap> profileReply = service.call(
            QStringLiteral("profileStatus"), profile);
        if (!profileReply.isValid()) {
            return printError(profileReply.error().message());
        }
        const QVariantMap profileValues = profileReply.value();
        output << "\nProfile: " << profileValues.value(QStringLiteral("profileName")).toString()
               << "\n  Backend: " << profileValues.value(QStringLiteral("backendName")).toString()
               << "\n  Availability: " << profileValues.value(QStringLiteral("availability")).toString()
               << "\n  Authenticated: "
               << (profileValues.value(QStringLiteral("authenticated")).toBool() ? "yes" : "no")
               << "\n  Sync: "
               << (profileValues.value(QStringLiteral("syncEnabled")).toBool() ? "enabled" : "paused")
               << "\n  Status: " << profileValues.value(QStringLiteral("syncStatus")).toString()
               << "\n  Progress: " << profileValues.value(QStringLiteral("syncProgress")).toInt()
               << "%\n";
        const QString error = profileValues.value(QStringLiteral("error")).toString();
        if (!error.isEmpty()) {
            output << "  Error: " << error << "\n";
        }
    }
    return 0;
}

/** Sends a synchronization or pause operation with optional D-Bus arguments. */
int callServiceMethod(const QString &method, const QVariantList &arguments = {})
{
    QDBusInterface service = serviceInterface();
    if (!service.isValid()) {
        return printError(service.lastError().message());
    }
    const QDBusMessage reply = service.callWithArgumentList(QDBus::AutoDetect,
                                                            method, arguments);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        return printError(reply.errorMessage());
    }
    QTextStream(stdout) << "Requested " << method << "." << Qt::endl;
    return 0;
}

/** Releases one path without ever issuing a remote deletion. */
int evictPath(const QString &profile, const QString &relativePath)
{
    QDBusInterface service = serviceInterface();
    if (!service.isValid()) {
        return printError(service.lastError().message());
    }
    return callServiceMethod(QStringLiteral("evictPath"), {profile, relativePath});
}

/** Controls the user-level systemd unit without requiring the tray process. */
int controlService(const QString &action)
{
    const QString systemctl = QStandardPaths::findExecutable(QStringLiteral("systemctl"));
    if (systemctl.isEmpty()) {
        return printError(QStringLiteral("systemctl was not found in PATH."));
    }
    QProcess process;
    process.start(systemctl, {QStringLiteral("--user"), action,
                              QString::fromLatin1(serviceUnit)});
    if (!process.waitForStarted()) {
        return printError(process.errorString());
    }
    process.waitForFinished(-1);
    const QByteArray standardOutput = process.readAllStandardOutput();
    const QByteArray standardError = process.readAllStandardError();
    if (!standardOutput.isEmpty()) {
        QTextStream(stdout) << QString::fromLocal8Bit(standardOutput);
    }
    if (!standardError.isEmpty()) {
        QTextStream(stderr) << QString::fromLocal8Bit(standardError);
    }
    return process.exitCode();
}

/** Requests the service-owned read-only FUSE view for one Graph profile. */
int mountProfile(const QString &profile)
{
    return callServiceMethod(QStringLiteral("mountProfile"), {profile});
}

/** Requests a service-owned unmount without touching synchronized content. */
int unmountProfile(const QString &profile)
{
    return callServiceMethod(QStringLiteral("unmountProfile"), {profile});
}
}

/** Implements the non-interactive DriveBeacon service command-line client. */
int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    application.setOrganizationDomain(QStringLiteral("io.github.clmates"));
    application.setOrganizationName(QStringLiteral("clmates"));
    application.setApplicationName(QStringLiteral("drivebeaconctl"));
    application.setApplicationVersion(QStringLiteral(DRIVEBEACON_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Control the headless DriveBeacon synchronization service."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(
        QStringLiteral("command"),
        QStringLiteral("status, sync, refresh-folders, reload-profiles, pause, resume, "
                       "pause-profile <name>, resume-profile <name>, or service "
                       "<start|stop|restart>; mount <name>; "
                       "unmount <name>; materialize <name> <relative-path>; "
                       "keep-local <name> <relative-path>; "
                       "evict <name> <relative-path>"));
    parser.process(application);

    const QStringList arguments = parser.positionalArguments();
    if (arguments.isEmpty()) {
        parser.showHelp(2);
    }
    const QString command = arguments.first();
    if (command == QStringLiteral("status") && arguments.size() == 1) {
        return printStatus();
    }
    if (command == QStringLiteral("sync") && arguments.size() == 1) {
        return callServiceMethod(QStringLiteral("synchronizeGraph"));
    }
    if (command == QStringLiteral("refresh-folders") && arguments.size() == 1) {
        return callServiceMethod(QStringLiteral("refreshGraphFolders"));
    }
    if (command == QStringLiteral("reload-profiles") && arguments.size() == 1) {
        return callServiceMethod(QStringLiteral("reloadProfiles"));
    }
    if (command == QStringLiteral("pause") && arguments.size() == 1) {
        return callServiceMethod(QStringLiteral("setGlobalSyncEnabled"), {false});
    }
    if (command == QStringLiteral("resume") && arguments.size() == 1) {
        return callServiceMethod(QStringLiteral("setGlobalSyncEnabled"), {true});
    }
    if ((command == QStringLiteral("pause-profile")
         || command == QStringLiteral("resume-profile"))
        && arguments.size() == 2) {
        return callServiceMethod(QStringLiteral("setProfileSyncEnabled"),
                                 {arguments.at(1), command == QStringLiteral("resume-profile")});
    }
    if (command == QStringLiteral("mount") && arguments.size() == 2) {
        return mountProfile(arguments.at(1));
    }
    if (command == QStringLiteral("unmount") && arguments.size() == 2) {
        return unmountProfile(arguments.at(1));
    }
    if (command == QStringLiteral("materialize") && arguments.size() == 3) {
        return callServiceMethod(QStringLiteral("materializeFile"),
                                 {arguments.at(1), arguments.at(2)});
    }
    if (command == QStringLiteral("keep-local") && arguments.size() == 3) {
        return callServiceMethod(QStringLiteral("keepLocalPath"),
                                 {arguments.at(1), arguments.at(2)});
    }
    if (command == QStringLiteral("evict") && arguments.size() == 3) {
        return evictPath(arguments.at(1), arguments.at(2));
    }
    if (command == QStringLiteral("service") && arguments.size() == 2) {
        const QString action = arguments.at(1);
        if (action == QStringLiteral("start") || action == QStringLiteral("stop")
            || action == QStringLiteral("restart")) {
            return controlService(action);
        }
    }
    return printError(QStringLiteral("Unknown command or arguments. Use --help for usage."));
}
