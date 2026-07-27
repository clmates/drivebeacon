// SPDX-License-Identifier: GPL-3.0-only

#include "deviceloginauth.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QHostAddress>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScopeGuard>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QTcpSocket>

namespace {
QNetworkRequest tokenRequest()
{
    QNetworkRequest request;
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
    return request;
}

const QUrl tokenUrl(QStringLiteral(
    "https://login.microsoftonline.com/common/oauth2/v2.0/token"));
const QUrl authorizeUrl(QStringLiteral(
    "https://login.microsoftonline.com/common/oauth2/v2.0/authorize"));
const QString scopes = QStringLiteral("offline_access Files.ReadWrite");
}

DeviceLoginAuth::DeviceLoginAuth(QObject *parent)
    : QObject(parent)
    , m_network(this)
    , m_callbackServer(this)
{
    connect(&m_callbackServer, &QTcpServer::newConnection, this,
            &DeviceLoginAuth::handleCallback);
}

void DeviceLoginAuth::start(const QString &clientId)
{
    cancel();
    if (clientId.trimmed().isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("A Microsoft application client ID is required."));
        return;
    }
    m_clientId = clientId.trimmed();
    if (m_callbackServer.listen(QHostAddress(QStringLiteral("127.0.0.1")), 0)) {
        m_redirectUri = QStringLiteral("http://localhost:%1/oauth/callback")
                            .arg(m_callbackServer.serverPort());
    } else {
        // Sandboxed or restricted environments can deny local sockets; retain the
        // manual native-client redirect as a functional fallback.
        m_redirectUri = QStringLiteral(
            "https://login.microsoftonline.com/common/oauth2/nativeclient");
    }
    m_state = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_codeVerifier = QUuid::createUuid().toString(QUuid::WithoutBraces)
                     + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QByteArray challenge = QCryptographicHash::hash(
        m_codeVerifier.toUtf8(), QCryptographicHash::Sha256)
                                     .toBase64(QByteArray::Base64UrlEncoding
                                               | QByteArray::OmitTrailingEquals);
    QUrl url(authorizeUrl);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("client_id"), m_clientId);
    query.addQueryItem(QStringLiteral("scope"), scopes);
    query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    query.addQueryItem(QStringLiteral("redirect_uri"), m_redirectUri);
    query.addQueryItem(QStringLiteral("prompt"), QStringLiteral("login"));
    query.addQueryItem(QStringLiteral("state"), m_state);
    query.addQueryItem(QStringLiteral("code_challenge"), QString::fromLatin1(challenge));
    query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    url.setQuery(query);
    Q_EMIT browserAuthorizationRequired(url);
}

void DeviceLoginAuth::submitAuthorizationResponse(const QString &responseUrl)
{
    const QUrl url(responseUrl.trimmed());
    if (!url.isValid() || url.query().isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("The browser response URL is invalid."));
        return;
    }

    const QUrlQuery query(url);
    if (!m_state.isEmpty() && query.queryItemValue(QStringLiteral("state")) != m_state) {
        Q_EMIT errorOccurred(QStringLiteral("The OAuth state does not match this login attempt."));
        return;
    }
    const QString error = query.queryItemValue(QStringLiteral("error"));
    if (!error.isEmpty()) {
        Q_EMIT errorOccurred(query.queryItemValue(QStringLiteral("error_description"),
                                                   QUrl::FullyDecoded));
        return;
    }
    const QString code = query.queryItemValue(QStringLiteral("code"));
    if (code.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("The browser response did not contain an authorization code."));
        return;
    }

    exchangeAuthorizationCode(code);
}

void DeviceLoginAuth::refresh(const QString &clientId, const QString &refreshToken)
{
    cancel();
    if (clientId.trimmed().isEmpty() || refreshToken.trimmed().isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("A client ID and refresh token are required."));
        return;
    }
    m_clientId = clientId.trimmed();
    m_refreshTokenForRenewal = refreshToken.trimmed();
    exchangeRefreshToken(refreshToken.trimmed());
}

void DeviceLoginAuth::exchangeAuthorizationCode(const QString &code)
{
    QNetworkRequest request(tokenUrl);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
    QUrlQuery tokenQuery;
    tokenQuery.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
    tokenQuery.addQueryItem(QStringLiteral("client_id"), m_clientId);
    tokenQuery.addQueryItem(QStringLiteral("code"), code);
    tokenQuery.addQueryItem(QStringLiteral("redirect_uri"), m_redirectUri);
    if (!m_codeVerifier.isEmpty()) {
        tokenQuery.addQueryItem(QStringLiteral("code_verifier"), m_codeVerifier);
    }
    auto *reply = m_network.post(request, tokenQuery.query(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply] { processTokenReply(reply); });
}

void DeviceLoginAuth::exchangeRefreshToken(const QString &refreshToken)
{
    QNetworkRequest request = tokenRequest();
    request.setUrl(tokenUrl);
    QUrlQuery tokenQuery;
    tokenQuery.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
    tokenQuery.addQueryItem(QStringLiteral("client_id"), m_clientId);
    tokenQuery.addQueryItem(QStringLiteral("refresh_token"), refreshToken);
    tokenQuery.addQueryItem(QStringLiteral("scope"), scopes);
    auto *reply = m_network.post(request, tokenQuery.query(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply] { processTokenReply(reply); });
}

void DeviceLoginAuth::processTokenReply(QNetworkReply *reply)
{
    const auto cleanup = qScopeGuard([reply] { reply->deleteLater(); });
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        Q_EMIT errorOccurred(reply->errorString().isEmpty()
                                 ? QStringLiteral("Microsoft returned invalid token JSON.")
                                 : reply->errorString());
        return;
    }
    const QJsonObject object = document.object();
    const QString error = object.value(QStringLiteral("error")).toString();
    if (!error.isEmpty()) {
        Q_EMIT errorOccurred(object.value(QStringLiteral("error_description"))
                                 .toString(error));
        return;
    }
    OAuthTokens tokens;
    tokens.accessToken = object.value(QStringLiteral("access_token")).toString();
    tokens.refreshToken = object.value(QStringLiteral("refresh_token")).toString();
    if (tokens.refreshToken.isEmpty()) {
        // Microsoft may omit a replacement refresh token during renewal.
        tokens.refreshToken = m_refreshTokenForRenewal;
    }
    tokens.expiresInSeconds = object.value(QStringLiteral("expires_in")).toInt();
    if (tokens.accessToken.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Microsoft returned no access token."));
        return;
    }
    // Access tokens are opaque credentials. They may be JWTs, but the client
    // must not inspect their structure; the resource server owns validation.
    cancel();
    Q_EMIT authenticated(tokens);
}

void DeviceLoginAuth::cancel()
{
    m_callbackServer.close();
    m_refreshTokenForRenewal.clear();
    m_state.clear();
    m_codeVerifier.clear();
}

void DeviceLoginAuth::handleCallback()
{
    while (m_callbackServer.hasPendingConnections()) {
        auto *socket = m_callbackServer.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
            const QByteArray request = socket->readAll();
            const qsizetype lineEnd = request.indexOf("\r\n");
            if (lineEnd < 0) {
                return;
            }

            const QList<QByteArray> parts = request.first(lineEnd).split(' ');
            if (parts.size() < 2) {
                socket->disconnectFromHost();
                return;
            }
            const QUrl callbackUrl(QString::fromUtf8(parts.at(1)));
            const QUrlQuery query(callbackUrl);
            const QString responseState = query.queryItemValue(QStringLiteral("state"));
            const QString code = query.queryItemValue(QStringLiteral("code"));
            const QString error = query.queryItemValue(QStringLiteral("error_description"),
                                                        QUrl::FullyDecoded);
            const bool validPath = callbackUrl.path() == QLatin1String("/oauth/callback");
            const bool validState = !m_state.isEmpty() && responseState == m_state;
            const bool success = validPath && validState && !code.isEmpty();
            const QByteArray body = success
                ? QByteArrayLiteral("<html><body>DriveBeacon authentication completed. "
                                    "You can close this window.</body></html>")
                : QByteArrayLiteral("<html><body>DriveBeacon authentication failed. "
                                    "Return to the application for details.</body></html>");
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                          "Connection: close\r\nContent-Length: "
                          + QByteArray::number(body.size()) + "\r\n\r\n" + body);
            socket->disconnectFromHost();
            if (!success) {
                Q_EMIT errorOccurred(error.isEmpty()
                                         ? QStringLiteral("Invalid OAuth callback response.")
                                         : error);
                return;
            }
            exchangeAuthorizationCode(code);
        });
    }
}
