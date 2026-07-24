// SPDX-License-Identifier: GPL-3.0-only

#include "tokenpayload.h"

#include <QJsonDocument>
#include <QJsonObject>

QByteArray serializeTokenPayload(const OAuthTokens &tokens)
{
    QJsonObject object;
    object.insert(QStringLiteral("refresh_token"), tokens.refreshToken);
    object.insert(QStringLiteral("expires_in"), tokens.expiresInSeconds);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

OAuthTokens deserializeTokenPayload(const QByteArray &data)
{
    OAuthTokens tokens;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return tokens;
    }
    const QJsonObject object = document.object();
    tokens.refreshToken = object.value(QStringLiteral("refresh_token")).toString();
    tokens.expiresInSeconds = object.value(QStringLiteral("expires_in")).toInt();
    return tokens;
}
