// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "deviceloginauth.h"

/** Serializes the non-secret OAuth metadata kept alongside a refresh token. */
QByteArray serializeTokenPayload(const OAuthTokens &tokens);
/** Parses a token payload without ever reconstructing an access token. */
OAuthTokens deserializeTokenPayload(const QByteArray &data);
