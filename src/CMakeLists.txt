# SPDX-License-Identifier: GPL-3.0-only

add_executable(drivebeacon)
set_target_properties(drivebeacon PROPERTIES OUTPUT_NAME DriveBeacon)

target_sources(drivebeacon PRIVATE
    activityevent.h
    activitymodel.cpp
    activitymodel.h
    activityparser.cpp
    activityparser.h
    deviceloginauth.cpp
    deviceloginauth.h
    drivebeaconserviceclient.cpp
    drivebeaconserviceclient.h
    graphclient.cpp
    graphclient.h
    journalreader.cpp
    journalreader.h
    main.cpp
    onedrivecontroller.cpp
    onedrivecontroller.h
    profiledialog.cpp
    profiledialog.h
    profilestore.cpp
    profilestore.h
    storagequota.cpp
    storagequota.h
    systemdmanager.cpp
    systemdmanager.h
    syncprofile.cpp
    syncprofile.h
    tokenstore.cpp
    tokenstore.h
    tokenpayload.cpp
    tokenpayload.h
)

add_executable(drivebeacon-service)
set_target_properties(drivebeacon-service PROPERTIES OUTPUT_NAME drivebeacon-service)
target_sources(drivebeacon-service PRIVATE
    activityevent.h
    activitymodel.cpp
    activitymodel.h
    activityparser.cpp
    activityparser.h
    deviceloginauth.cpp
    deviceloginauth.h
    drivebeaconservice.cpp
    drivebeaconservice.h
    graphclient.cpp
    graphclient.h
    journalreader.cpp
    journalreader.h
    onedrivecontroller.cpp
    onedrivecontroller.h
    profilestore.cpp
    profilestore.h
    storagequota.cpp
    storagequota.h
    systemdmanager.cpp
    systemdmanager.h
    syncprofile.cpp
    syncprofile.h
    service_main.cpp
    tokenstore.cpp
    tokenstore.h
    tokenpayload.cpp
    tokenpayload.h
)

target_compile_features(drivebeacon PRIVATE cxx_std_20)
target_compile_definitions(drivebeacon PRIVATE DRIVEBEACON_VERSION="${PROJECT_VERSION}")
target_link_libraries(drivebeacon PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Gui
    Qt6::Network
    Qt6::Widgets
    KF6::CoreAddons
    KF6::I18n
    KF6::StatusNotifierItem
    KF6::Wallet
    KF6::XmlGui
    systemd
)

target_compile_features(drivebeacon-service PRIVATE cxx_std_20)
target_compile_definitions(drivebeacon-service PRIVATE DRIVEBEACON_VERSION="${PROJECT_VERSION}")
target_link_libraries(drivebeacon-service PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Network
    KF6::CoreAddons
    KF6::I18n
    KF6::Wallet
    systemd
)

# Dolphin loads this action provider from KIO's file-item-action namespace.
# The provider delegates all state changes to drivebeaconctl, so credentials and
# synchronization policy remain owned by the headless DriveBeacon service.
kcoreaddons_add_plugin(drivebeacon_fileitemaction
    SOURCES drivebeaconfileitemaction.cpp drivebeaconmountstate.h drivebeaconfileitemaction.json
    INSTALL_NAMESPACE "kf6/kfileitemaction"
)
target_compile_definitions(drivebeacon_fileitemaction PRIVATE
    DRIVEBEACON_CTL_PATH="${CMAKE_INSTALL_FULL_BINDIR}/drivebeaconctl"
)
target_link_libraries(drivebeacon_fileitemaction PRIVATE
    Qt6::Core
    KF6::CoreAddons
    KF6::I18n
    KF6::KIOWidgets
)

# Dolphin asks overlay providers for lightweight visual state without opening
# the selected file. This provider reads the persisted cache index only.
kcoreaddons_add_plugin(drivebeacon_overlayicon
    SOURCES drivebeaconoverlayicon.cpp drivebeaconmountstate.h
    INSTALL_NAMESPACE "kf6/overlayicon"
)
target_link_libraries(drivebeacon_overlayicon PRIVATE
    Qt6::Core
    KF6::KIOCore
)
install(FILES ../data/drivebeaconfileitemaction.desktop
    DESTINATION ${KDE_INSTALL_DATADIR}/kservices6
)

add_executable(drivebeaconctl drivebeaconctl.cpp)
target_compile_features(drivebeaconctl PRIVATE cxx_std_20)
target_compile_definitions(drivebeaconctl PRIVATE DRIVEBEACON_VERSION="${PROJECT_VERSION}")
target_link_libraries(drivebeaconctl PRIVATE Qt6::Core Qt6::DBus)

# The FUSE helper is deliberately a separate process. The synchronization
# service remains the owner of Graph credentials and network state, while this
# process only translates filesystem operations into D-Bus requests.
add_executable(drivebeacon-fs drivebeaconfs.cpp)
target_compile_features(drivebeacon-fs PRIVATE cxx_std_20)
target_compile_definitions(drivebeacon-fs PRIVATE DRIVEBEACON_VERSION="${PROJECT_VERSION}")
target_link_libraries(drivebeacon-fs PRIVATE Qt6::Core Qt6::DBus PkgConfig::FUSE3)

ecm_create_qm_loader(drivebeacon drivebeacon)

install(TARGETS drivebeacon ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
install(TARGETS drivebeacon-service ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
install(TARGETS drivebeaconctl ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
install(TARGETS drivebeacon-fs ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
configure_file(
    "${CMAKE_SOURCE_DIR}/data/drivebeacon-service.service.in"
    "${CMAKE_CURRENT_BINARY_DIR}/drivebeacon-service.service"
    @ONLY
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/drivebeacon-service.service"
    DESTINATION ${KDE_INSTALL_DATADIR}/systemd/user
)
install(PROGRAMS "${CMAKE_BINARY_DIR}/share/applications/io.github.clmates.drivebeacon.desktop"
    DESTINATION ${KDE_INSTALL_APPDIR}
)
install(FILES ../data/io.github.clmates.drivebeacon.metainfo.xml
    DESTINATION ${KDE_INSTALL_METAINFODIR}
)

add_custom_target(run
    COMMAND ${CMAKE_COMMAND} -E env
        "XDG_DATA_DIRS=${CMAKE_BINARY_DIR}/share:$ENV{XDG_DATA_DIRS}"
        "$<TARGET_FILE:drivebeacon>"
    DEPENDS drivebeacon
    USES_TERMINAL
)
