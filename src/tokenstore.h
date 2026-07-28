// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "tokenpayload.h"

#include <QString>

/** Stores Graph refresh tokens in the user's encrypted KDE Wallet. */
class TokenStore final
{
public:
    /** Reads the refresh token for a profile; false means no usable token exists. */
    static bool load(const QString &profileName, OAuthTokens *tokens, QString *error = nullptr);
    /** Writes only the refresh token and metadata; access tokens remain memory-only. */
    static bool save(const QString &profileName, const OAuthTokens &tokens,
                     QString *error = nullptr);
    /** Removes only the profile's wallet entry; it never contacts Microsoft Graph. */
    static bool remove(const QString &profileName, QString *error = nullptr);
    /** Stable JSON representation used inside the wallet and by deterministic tests. */
    static QByteArray serialize(const OAuthTokens &tokens);
    /** Parses the representation created by serialize(). */
    static OAuthTokens deserialize(const QByteArray &data);
};
