// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QTcpServer>

/** OAuth tokens returned by the Microsoft identity platform. */
struct OAuthTokens {
    /** Short-lived bearer token kept only in process memory. */
    QString accessToken;
    /** Long-lived renewal credential persisted in KDE Wallet. */
    QString refreshToken;
    /** Provider-reported lifetime of the access token, for UI/state consumers. */
    int expiresInSeconds = 0;
};

Q_DECLARE_METATYPE(OAuthTokens)

/** Implements interactive OAuth for a public desktop client without storing credentials. */
class DeviceLoginAuth final : public QObject
{
    Q_OBJECT

public:
    /** Creates an idle authentication session for a personal Microsoft account. */
    explicit DeviceLoginAuth(QObject *parent = nullptr);

    /** Starts browser authorization with the supplied public application client ID. */
    void start(const QString &clientId);
    /** Exchanges the authorization-code redirect URL copied from the browser. */
    void submitAuthorizationResponse(const QString &responseUrl);
    /** Exchanges a stored refresh token for a new short-lived access token. */
    void refresh(const QString &clientId, const QString &refreshToken);
    /** Cancels the active browser exchange and clears ephemeral session state. */
    void cancel();

Q_SIGNALS:
    /** Requests that the UI open this browser authorization URL. */
    void browserAuthorizationRequired(const QUrl &authorizationUrl);
    /** Emitted when Microsoft returns usable access and refresh tokens. */
    void authenticated(const OAuthTokens &tokens);
    /** Emitted when the flow expires, is denied, or encounters a transport error. */
    void errorOccurred(const QString &message);

private:
    /** Exchanges the PKCE authorization code for Graph tokens. */
    void exchangeAuthorizationCode(const QString &code);
    /** Exchanges a wallet-backed refresh token for a new access token. */
    void exchangeRefreshToken(const QString &refreshToken);
    /** Parses token responses and preserves a refresh token omitted during renewal. */
    void processTokenReply(QNetworkReply *reply);
    /** Validates and answers the loopback OAuth callback request. */
    void handleCallback();

    QNetworkAccessManager m_network;
    QTcpServer m_callbackServer;
    /** Ephemeral PKCE session state; credentials are never written here. */
    QString m_clientId;
    QString m_refreshTokenForRenewal;
    QString m_redirectUri = QStringLiteral(
        "https://login.microsoftonline.com/common/oauth2/nativeclient");
    QString m_state;
    QString m_codeVerifier;
};
