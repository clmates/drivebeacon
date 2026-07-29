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
    /** Restores empty remote folders so cache directories are not uploaded. */
    void restoresPersistedEmptyFolder();
    /** Baselines written by the short-lived three-field local format remain usable. */
    void acceptsLegacyLocalBaseline();
    /** A KeepLocal folder policy must replace conflicting descendant policies. */
    void parentKeepLocalClearsDescendantReleasePolicy();
    /** Retry-After values are bounded and support HTTP-date form. */
    void parsesRetryAfterValues();
    /** Provider backoff is never shortened below the requested Retry-After. */
    void combinesRetryDelays();
    /** Invalid delta cursors are recognized as recoverable baseline failures. */
    void recognizesInvalidDeltaCursor();
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

void GraphClientStateTest::restoresPersistedEmptyFolder()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    GraphClient client;
    client.initializeLocalMonitoring({},
                                     {QStringLiteral("folder-a\tDocumentos\tetag-a\tfolder")},
                                     directory.path());

    const QVariantList entries = client.remoteEntries();
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().toMap().value(QStringLiteral("path")).toString(),
             QStringLiteral("Documentos"));
    QVERIFY(entries.first().toMap().value(QStringLiteral("folder")).toBool());
    QCOMPARE(client.remotePaths(),
             QStringList({QStringLiteral("folder-a\tDocumentos\tetag-a\tfolder")}));
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

void GraphClientStateTest::combinesRetryDelays()
{
    QCOMPARE(graphRetryDelaySeconds(1, 0), 5);
    QCOMPARE(graphRetryDelaySeconds(17, 0), 17);
    QCOMPARE(graphRetryDelaySeconds(3600, 6), 3600);
    QCOMPARE(graphRetryDelaySeconds(1, 99), 320);
}

void GraphClientStateTest::recognizesInvalidDeltaCursor()
{
    QVERIFY(graphDeltaCursorIsInvalid(
        QStringLiteral("Graph (400): Resource not found for the segment 'delta'.")));
    QVERIFY(!graphDeltaCursorIsInvalid(QStringLiteral("Graph (400): Bad request.")));
    QVERIFY(!graphDeltaCursorIsInvalid(QStringLiteral("Graph (404): delta not found.")));
}

QTEST_MAIN(GraphClientStateTest)
#include "graphclient_state_test.moc"
