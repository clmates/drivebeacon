# SPDX-License-Identifier: GPL-3.0-only

add_executable(drivebeacon)
set_target_properties(drivebeacon PROPERTIES OUTPUT_NAME DriveBeacon)

target_sources(drivebeacon PRIVATE
    activityevent.h
    activitymodel.cpp
    activitymodel.h
    activityparser.cpp
    activityparser.h
    journalreader.cpp
    journalreader.h
    main.cpp
    onedrivecontroller.cpp
    onedrivecontroller.h
    systemdmanager.cpp
    systemdmanager.h
)

target_compile_features(drivebeacon PRIVATE cxx_std_20)
target_compile_definitions(drivebeacon PRIVATE DRIVEBEACON_VERSION="${PROJECT_VERSION}")
target_link_libraries(drivebeacon PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Gui
    Qt6::Widgets
    KF6::CoreAddons
    KF6::I18n
    KF6::StatusNotifierItem
    KF6::XmlGui
)

ecm_create_qm_loader(drivebeacon drivebeacon)

install(TARGETS drivebeacon ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
install(PROGRAMS ../data/io.github.clmates.drivebeacon.desktop
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
