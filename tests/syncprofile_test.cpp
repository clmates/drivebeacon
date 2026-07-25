// SPDX-License-Identifier: GPL-3.0-only

#include "storagequota.h"
#include "profilestore.h"
#include "syncprofile.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

/** Tests stable profile values and Graph quota normalization. */
class SyncProfileTest final : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void roundTripsBackendNames();
    void roundTripsAvailabilityNames();
    void parsesGraphQuota();
    void marksMissingQuotaValuesUnknown();
    void storesProfilesIndependently();
    void storesIndividualAndGlobalSyncState();

private:
    QTemporaryDir m_configDirectory;
};

void SyncProfileTest::initTestCase()
{
    QVERIFY(m_configDirectory.isValid());
    qputenv("XDG_CONFIG_HOME", m_configDirectory.path().toUtf8());
    QCoreApplication::setOrganizationName(QStringLiteral("DriveBeaconTest"));
    QCoreApplication::setApplicationName(QStringLiteral("DriveBeaconTest"));
}

void SyncProfileTest::roundTripsBackendNames()
{
    QCOMPARE(syncBackendFromName(syncBackendName(SyncBackend::AbrauneggJournal)),
             SyncBackend::AbrauneggJournal);
    QCOMPARE(syncBackendFromName(syncBackendName(SyncBackend::MicrosoftGraph)),
             SyncBackend::MicrosoftGraph);
    QCOMPARE(syncBackendFromName(QStringLiteral("unknown")), SyncBackend::AbrauneggJournal);
}

void SyncProfileTest::roundTripsAvailabilityNames()
{
    QCOMPARE(localAvailabilityFromName(localAvailabilityName(LocalAvailability::KeepLocal)),
             LocalAvailability::KeepLocal);
    QCOMPARE(localAvailabilityFromName(localAvailabilityName(LocalAvailability::RemoteOnly)),
             LocalAvailability::RemoteOnly);
    QCOMPARE(localAvailabilityFromName(localAvailabilityName(LocalAvailability::OnDemand)),
             LocalAvailability::OnDemand);
}

void SyncProfileTest::parsesGraphQuota()
{
    const auto updatedAt = QDateTime::fromSecsSinceEpoch(1234, QTimeZone::UTC);
    const StorageQuota quota = StorageQuota::fromGraphObject(
        QJsonObject{{QStringLiteral("total"), 1000},
                    {QStringLiteral("used"), 250},
                    {QStringLiteral("remaining"), 750}},
        updatedAt);

    QCOMPARE(quota.total, 1000);
    QCOMPARE(quota.used, 250);
    QCOMPARE(quota.remaining, 750);
    QCOMPARE(quota.lastUpdated, updatedAt);
    QVERIFY(!quota.stale);
}

void SyncProfileTest::marksMissingQuotaValuesUnknown()
{
    const StorageQuota quota = StorageQuota::fromGraphObject(
        QJsonObject{{QStringLiteral("used"), 12}}, QDateTime::currentDateTimeUtc());

    QCOMPARE(quota.total, qint64(-1));
    QCOMPARE(quota.used, qint64(12));
    QCOMPARE(quota.remaining, qint64(-1));
}

void SyncProfileTest::storesProfilesIndependently()
{
    ProfileStore store;
    SyncProfile abraunegg;
    abraunegg.name = QStringLiteral("abraunegg");
    abraunegg.localDirectory = QStringLiteral("/tmp/OneDrive");
    store.save(abraunegg);

    SyncProfile graph;
    graph.name = QStringLiteral("graph-test");
    graph.backend = SyncBackend::MicrosoftGraph;
    graph.localDirectory = QStringLiteral("/tmp/OneDrive-Graph-Test");
    graph.concurrentDownloads = 4;
    graph.concurrentUploads = 3;
    graph.concurrentLargeTransfers = 2;
    graph.graphDeltaLink = QStringLiteral("https://graph.example/delta");
    graph.graphLocalSignatures = {QStringLiteral("a\tsha")};
    graph.graphRemotePaths = {QStringLiteral("id\tDocumentos/a.txt\te")};
    graph.graphSyncedIncludedFolders = {QStringLiteral("Documentos")};
    store.save(graph);
    store.setActiveProfileName(graph.name);

    QCOMPARE(store.profileNames(), QStringList({QStringLiteral("abraunegg"),
                                                QStringLiteral("graph-test")}));
    QCOMPARE(store.load(QStringLiteral("abraunegg")).backend,
             SyncBackend::AbrauneggJournal);
    QCOMPARE(store.load().name, QStringLiteral("graph-test"));
    QCOMPARE(store.load().localDirectory, QStringLiteral("/tmp/OneDrive-Graph-Test"));
    QCOMPARE(store.load().concurrentDownloads, 4);
    QCOMPARE(store.load().concurrentUploads, 3);
    QCOMPARE(store.load().concurrentLargeTransfers, 2);
    QCOMPARE(store.load().graphDeltaLink, QStringLiteral("https://graph.example/delta"));
    QCOMPARE(store.load().graphLocalSignatures, QStringList({QStringLiteral("a\tsha")}));
    QCOMPARE(store.load().graphRemotePaths,
             QStringList({QStringLiteral("id\tDocumentos/a.txt\te")}));
    QCOMPARE(store.load().graphSyncedIncludedFolders,
             QStringList({QStringLiteral("Documentos")}));
}

void SyncProfileTest::storesIndividualAndGlobalSyncState()
{
    ProfileStore store;
    SyncProfile profile;
    profile.name = QStringLiteral("paused-account");
    profile.syncEnabled = false;
    store.save(profile);

    QCOMPARE(store.load(profile.name).syncEnabled, false);
    QCOMPARE(store.globalSyncEnabled(), true);
    store.setGlobalSyncEnabled(false);
    QCOMPARE(store.globalSyncEnabled(), false);

    // Global pause is independent from the per-profile switch and must not
    // rewrite the profile's own state or its persisted synchronization data.
    QCOMPARE(store.load(profile.name).syncEnabled, false);
}

QTEST_MAIN(SyncProfileTest)
#include "syncprofile_test.moc"
