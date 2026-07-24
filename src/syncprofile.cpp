// SPDX-License-Identifier: GPL-3.0-only

#include "syncprofile.h"

QString syncBackendName(SyncBackend backend)
{
    return backend == SyncBackend::MicrosoftGraph ? QStringLiteral("graph")
                                                  : QStringLiteral("abraunegg-journal");
}

SyncBackend syncBackendFromName(const QString &name)
{
    return name == QLatin1String("graph") ? SyncBackend::MicrosoftGraph
                                           : SyncBackend::AbrauneggJournal;
}

QString localAvailabilityName(LocalAvailability availability)
{
    switch (availability) {
    case LocalAvailability::RemoteOnly:
        return QStringLiteral("remote-only");
    case LocalAvailability::OnDemand:
        return QStringLiteral("on-demand");
    case LocalAvailability::KeepLocal:
        return QStringLiteral("keep-local");
    }
    return QStringLiteral("keep-local");
}

LocalAvailability localAvailabilityFromName(const QString &name)
{
    if (name == QLatin1String("remote-only")) {
        return LocalAvailability::RemoteOnly;
    }
    if (name == QLatin1String("on-demand")) {
        return LocalAvailability::OnDemand;
    }
    return LocalAvailability::KeepLocal;
}
