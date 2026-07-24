// SPDX-License-Identifier: GPL-3.0-only

#include "deviceloginauth.h"

#include <QSignalSpy>
#include <QTest>

/** Regression tests for safe, deterministic Device Code flow boundaries. */
class DeviceLoginAuthTest final : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void rejectsMissingClientId();
    void emitsBrowserAuthorizationUrl();
};

void DeviceLoginAuthTest::rejectsMissingClientId()
{
    DeviceLoginAuth auth;
    QSignalSpy errors(&auth, &DeviceLoginAuth::errorOccurred);

    auth.start({});

    QCOMPARE(errors.size(), 1);
    QCOMPARE(errors.constFirst().constFirst().toString(),
             QStringLiteral("A Microsoft application client ID is required."));
}

void DeviceLoginAuthTest::emitsBrowserAuthorizationUrl()
{
    DeviceLoginAuth auth;
    QSignalSpy authorization(&auth, &DeviceLoginAuth::browserAuthorizationRequired);

    auth.start(QStringLiteral("client-id"));

    QCOMPARE(authorization.size(), 1);
    const QUrl url = authorization.constFirst().constFirst().toUrl();
    QCOMPARE(url.path(), QStringLiteral("/common/oauth2/v2.0/authorize"));
    QVERIFY(url.query().contains(QStringLiteral("client_id=client-id")));
    QVERIFY(url.query().contains(QStringLiteral("redirect_uri=")));
}

QTEST_MAIN(DeviceLoginAuthTest)
#include "deviceloginauth_test.moc"
