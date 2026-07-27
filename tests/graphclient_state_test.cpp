// SPDX-License-Identifier: GPL-3.0-only

#include "graphclient.h"
#include "graphretrypolicy.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QTest>

/** Protects the persisted Graph baseline format from restart regressions. */
class GraphClientStateTest final : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    /** Local hashes must not contain the remote eTag field. */
    void roundTripsSeparateBaselines();
    /** Restores persisted remote sizes needed by on-demand materialization. */
    void restoresPersistedRemoteSize();
    /** Baselines written by the short-lived three-field local format remain usable. */
    void acceptsLegacyLocalBaseline();
    /** A KeepLocal folder policy must replace conflicting descendant policies. */
    void parentKeepLocalClearsDescendantReleasePolicy();
    /** Retry-After values are bounded and support HTTP-date form. */
    void parsesRetryAfterValues();
};

void GraphClientStateTest::roundTripsSeparateBaselines()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    GraphClient client;
    client.initializeLocalMonitoring({QStringLiteral("Documentos/a.txt\thash-a")},
                                     {QStringLiteral("item-a\tDocumentos/a.txt\tetag-a")},
                                     directory.path());

    QCOMPARE(client.localSignatures(), QStringList({QStringLiteral("Documentos/a.txt\thash-a")}));
    QCOMPARE(client.remotePaths(), QStringList({QStringLiteral("item-a\tDocumentos/a.txt\tetag-a")}));
}

void GraphClientStateTest::restoresPersistedRemoteSize()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    GraphClient client;
    client.initializeLocalMonitoring({},
                                     {QStringLiteral("item-a\tDocumentos/a.txt\tetag-a\t123")},
                                     directory.path());

    QCOMPARE(client.remotePaths(),
             QStringList({QStringLiteral("item-a\tDocumentos/a.txt\tetag-a\t123")}));
    const QVariantList entries = client.remoteEntries();
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().toMap().value(QStringLiteral("size")).toLongLong(), 123LL);
}

void GraphClientStateTest::acceptsLegacyLocalBaseline()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    GraphClient client;
    client.initializeLocalMonitoring({QStringLiteral("Documentos/a.txt\thash-a\t")}, {},
                                     directory.path());

    QCOMPARE(client.localSignatures(), QStringList({QStringLiteral("Documentos/a.txt\thash-a")}));
}

void GraphClientStateTest::parentKeepLocalClearsDescendantReleasePolicy()
{
    GraphClient client;
    client.setPathPolicies({QStringLiteral("Documentos/Videos\tremote-only")});

    client.setPathPolicy(QStringLiteral("Documentos"), LocalAvailability::KeepLocal);

    QCOMPARE(client.pathPolicies(), QStringList({QStringLiteral("Documentos\tkeep-local")}));
}

void GraphClientStateTest::parsesRetryAfterValues()
{
    const QDateTime now = QDateTime::fromString(
        QStringLiteral("Tue, 28 Jul 2026 12:00:00 +0000"), Qt::RFC2822Date);
    QCOMPARE(graphRetryAfterSeconds(QByteArrayLiteral("17"), now), 17);
    QCOMPARE(graphRetryAfterSeconds(QByteArrayLiteral("0"), now), 1);
    QCOMPARE(graphRetryAfterSeconds(QByteArrayLiteral("99999"), now), 3600);
    QCOMPARE(graphRetryAfterSeconds(QByteArrayLiteral("Tue, 28 Jul 2026 12:00:12 GMT"), now), 12);
    QCOMPARE(graphRetryAfterSeconds(QByteArrayLiteral("invalid"), now), 5);
}

QTEST_MAIN(GraphClientStateTest)
#include "graphclient_state_test.moc"
