// SPDX-License-Identifier: GPL-3.0-only

#include "tokenpayload.h"

#include <QTest>

/** Verifies that the wallet payload never needs to contain an access token. */
class TokenStoreTest final : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void roundTripsRefreshTokenOnly();
};

void TokenStoreTest::roundTripsRefreshTokenOnly()
{
    OAuthTokens original;
    original.accessToken = QStringLiteral("short-lived-access");
    original.refreshToken = QStringLiteral("long-lived-refresh");
    original.expiresInSeconds = 3600;

    const QByteArray payload = serializeTokenPayload(original);
    QVERIFY(!payload.contains("short-lived-access"));
    const OAuthTokens restored = deserializeTokenPayload(payload);
    QCOMPARE(restored.accessToken, QString());
    QCOMPARE(restored.refreshToken, original.refreshToken);
    QCOMPARE(restored.expiresInSeconds, original.expiresInSeconds);
}

QTEST_MAIN(TokenStoreTest)
#include "tokenstore_test.moc"
