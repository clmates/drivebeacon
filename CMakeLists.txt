# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.20)

project(drivebeacon VERSION 1.0.10 LANGUAGES CXX)

find_package(ECM 6.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(CTest)
include(FeatureSummary)
include(ECMPoQmTools)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(KDEInstallDirs)
include(KDECMakeSettings)

# Resolve the executable through the selected install prefix. Desktop launchers
# do not inherit the interactive shell PATH, so a user-local installation must
# not accidentally start an older system-wide DriveBeacon binary.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/share/applications")
configure_file(
    "${CMAKE_SOURCE_DIR}/data/io.github.clmates.drivebeacon.desktop"
    "${CMAKE_BINARY_DIR}/share/applications/io.github.clmates.drivebeacon.desktop"
    @ONLY
)

find_package(Qt6 6.7 REQUIRED COMPONENTS
    Core
    DBus
    Gui
    Network
    Test
    Widgets
)
find_package(KF6 6.0 REQUIRED COMPONENTS
    CoreAddons
    I18n
    KIO
    StatusNotifierItem
    Wallet
    XmlGui
)
include("${KF6CoreAddons_DIR}/KF6CoreAddonsMacros.cmake")
find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3)

add_subdirectory(src)

ecm_install_po_files_as_qm(po)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)
