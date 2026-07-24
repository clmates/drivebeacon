# SPDX-License-Identifier: GPL-3.0-only

qt_add_executable(activityparser_test
    activityparser_test.cpp
    ../src/activityparser.cpp
)
target_include_directories(activityparser_test PRIVATE ../src)
target_compile_features(activityparser_test PRIVATE cxx_std_20)
target_link_libraries(activityparser_test PRIVATE Qt6::Core Qt6::Test)

add_test(NAME activityparser_test COMMAND activityparser_test)

qt_add_executable(syncprofile_test
    syncprofile_test.cpp
    ../src/profilestore.cpp
    ../src/storagequota.cpp
    ../src/syncprofile.cpp
)
target_include_directories(syncprofile_test PRIVATE ../src)
target_compile_features(syncprofile_test PRIVATE cxx_std_20)
target_link_libraries(syncprofile_test PRIVATE Qt6::Core Qt6::Test)

add_test(NAME syncprofile_test COMMAND syncprofile_test)

qt_add_executable(deviceloginauth_test
    deviceloginauth_test.cpp
    ../src/deviceloginauth.cpp
)
target_include_directories(deviceloginauth_test PRIVATE ../src)
target_compile_features(deviceloginauth_test PRIVATE cxx_std_20)
target_link_libraries(deviceloginauth_test PRIVATE Qt6::Core Qt6::Network Qt6::Test)

add_test(NAME deviceloginauth_test COMMAND deviceloginauth_test)

qt_add_executable(tokenstore_test
    tokenstore_test.cpp
    ../src/deviceloginauth.cpp
    ../src/tokenpayload.cpp
)
target_include_directories(tokenstore_test PRIVATE ../src)
target_compile_features(tokenstore_test PRIVATE cxx_std_20)
target_link_libraries(tokenstore_test PRIVATE Qt6::Core Qt6::Network Qt6::Test)

add_test(NAME tokenstore_test COMMAND tokenstore_test)

qt_add_executable(graphclient_state_test
    graphclient_state_test.cpp
    ../src/graphclient.cpp
    ../src/storagequota.cpp
)
target_include_directories(graphclient_state_test PRIVATE ../src)
target_compile_features(graphclient_state_test PRIVATE cxx_std_20)
target_link_libraries(graphclient_state_test PRIVATE Qt6::Core Qt6::Network Qt6::Test systemd)

add_test(NAME graphclient_state_test COMMAND graphclient_state_test)

add_test(NAME drivebeaconctl_help COMMAND $<TARGET_FILE:drivebeaconctl> --help)

find_program(DESKTOP_FILE_VALIDATE_EXECUTABLE desktop-file-validate)
if(DESKTOP_FILE_VALIDATE_EXECUTABLE)
    add_test(NAME desktopfile_test
        COMMAND ${DESKTOP_FILE_VALIDATE_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/data/io.github.clmates.drivebeacon.desktop
    )
endif()

find_program(APPSTREAMCLI_EXECUTABLE appstreamcli)
if(APPSTREAMCLI_EXECUTABLE)
    add_test(NAME metainfo_test
        COMMAND ${APPSTREAMCLI_EXECUTABLE} validate --no-net
            ${CMAKE_SOURCE_DIR}/data/io.github.clmates.drivebeacon.metainfo.xml
    )
endif()
