// SPDX-License-Identifier: GPL-3.0-only

#include "graphclient.h"

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
    /** Baselines written by the short-lived three-field local format remain usable. */
    void acceptsLegacyLocalBaseline();
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

void GraphClientStateTest::acceptsLegacyLocalBaseline()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    GraphClient client;
    client.initializeLocalMonitoring({QStringLiteral("Documentos/a.txt\thash-a\t")}, {},
                                     directory.path());

    QCOMPARE(client.localSignatures(), QStringList({QStringLiteral("Documentos/a.txt\thash-a")}));
}

QTEST_MAIN(GraphClientStateTest)
#include "graphclient_state_test.moc"
