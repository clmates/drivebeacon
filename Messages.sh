#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

: "${XGETTEXT:=xgettext}"
: "${podir:=po}"

mkdir -p "${podir}"

cpp_sources=(
    src/activitymodel.cpp
    src/activityparser.cpp
    src/journalreader.cpp
    src/main.cpp
    src/onedrivecontroller.cpp
    src/systemdmanager.cpp
)

qml_sources=(
    src/qml/Main.qml
)

"${XGETTEXT}" --from-code=UTF-8 --kde --language=C++ \
    --keyword=i18n:1 \
    --keyword=i18nc:1c,2 \
    --keyword=i18np:1,2 \
    --keyword=i18ncp:1c,2,3 \
    "${cpp_sources[@]}" \
    --package-name=drivebeacon \
    --package-version=0.1.1 \
    --output="${podir}/drivebeacon.pot"

"${XGETTEXT}" --from-code=UTF-8 --language=JavaScript --join-existing \
    --keyword=i18n:1 \
    --keyword=i18nc:1c,2 \
    --keyword=i18np:1,2 \
    --keyword=i18ncp:1c,2,3 \
    "${qml_sources[@]}" \
    --package-name=drivebeacon \
    --package-version=0.1.1 \
    --output="${podir}/drivebeacon.pot"
