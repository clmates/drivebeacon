// SPDX-License-Identifier: GPL-3.0-only

#include "tokenstore.h"

#include <KWallet>

namespace {
/** Wallet namespace shared by all profile credential entries. */
const QString walletFolder = QStringLiteral("DriveBeacon");
const QString walletName = QStringLiteral("DriveBeacon");

QString entryName(const QString &profileName)
{
    return QStringLiteral("graph/%1").arg(profileName);
}

/** Opens and selects the DriveBeacon wallet folder, reporting UI-safe errors. */
KWallet::Wallet *openWallet(QString *error)
{
    if (!KWallet::Wallet::isEnabled()) {
        if (error) {
            *error = QStringLiteral("The KDE Wallet service is disabled.");
        }
        return nullptr;
    }
    auto *wallet = KWallet::Wallet::openWallet(walletName, 0, KWallet::Wallet::Synchronous);
    if (!wallet || !wallet->isOpen()) {
        delete wallet;
        if (error) {
            *error = QStringLiteral("DriveBeacon could not open the KDE Wallet.");
        }
        return nullptr;
    }
    if (!wallet->hasFolder(walletFolder) && !wallet->createFolder(walletFolder)) {
        delete wallet;
        if (error) {
            *error = QStringLiteral("DriveBeacon could not create its secure wallet folder.");
        }
        return nullptr;
    }
    if (!wallet->setFolder(walletFolder)) {
        delete wallet;
        if (error) {
            *error = QStringLiteral("DriveBeacon could not select its secure wallet folder.");
        }
        return nullptr;
    }
    return wallet;
}
}

QByteArray TokenStore::serialize(const OAuthTokens &tokens)
{
    return serializeTokenPayload(tokens);
}

OAuthTokens TokenStore::deserialize(const QByteArray &data)
{
    return deserializeTokenPayload(data);
}

bool TokenStore::load(const QString &profileName, OAuthTokens *tokens, QString *error)
{
    if (!tokens || profileName.trimmed().isEmpty()) {
        if (error) {
            *error = QStringLiteral("A profile name is required to load credentials.");
        }
        return false;
    }
    auto *wallet = openWallet(error);
    if (!wallet) {
        return false;
    }
    QString value;
    const bool found = wallet->readPassword(entryName(profileName), value) == 0;
    delete wallet;
    if (found) {
        *tokens = deserialize(value.toUtf8());
    }
    return found && !tokens->refreshToken.isEmpty();
}

bool TokenStore::save(const QString &profileName, const OAuthTokens &tokens, QString *error)
{
    if (profileName.trimmed().isEmpty() || tokens.refreshToken.isEmpty()) {
        if (error) {
            *error = QStringLiteral("A profile name and refresh token are required.");
        }
        return false;
    }
    auto *wallet = openWallet(error);
    if (!wallet) {
        return false;
    }
    const bool saved = wallet->writePassword(entryName(profileName),
                                             QString::fromUtf8(serialize(tokens))) == 0;
    delete wallet;
    if (!saved && error) {
        *error = QStringLiteral("The KDE Wallet rejected the Graph credentials.");
    }
    return saved;
}

bool TokenStore::remove(const QString &profileName, QString *error)
{
    if (profileName.trimmed().isEmpty()) {
        if (error) {
            *error = QStringLiteral("A profile name is required to remove credentials.");
        }
        return false;
    }
    auto *wallet = openWallet(error);
    if (!wallet) {
        return false;
    }
    const int result = wallet->removeEntry(entryName(profileName));
    delete wallet;
    if (result != 0 && error) {
        *error = QStringLiteral("The KDE Wallet rejected the profile credentials removal.");
    }
    return result == 0;
}
