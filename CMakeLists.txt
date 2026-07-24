# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.20)

project(drivebeacon VERSION 0.1.3 LANGUAGES CXX)

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/share/applications")
configure_file(
    "${CMAKE_SOURCE_DIR}/data/io.github.clmates.drivebeacon.desktop"
    "${CMAKE_BINARY_DIR}/share/applications/io.github.clmates.drivebeacon.desktop"
    COPYONLY
)

find_package(ECM 6.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(CTest)
include(FeatureSummary)
include(ECMPoQmTools)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(KDEInstallDirs)
include(KDECMakeSettings)

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
    StatusNotifierItem
    Wallet
    XmlGui
)

add_subdirectory(src)

ecm_install_po_files_as_qm(po)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)
