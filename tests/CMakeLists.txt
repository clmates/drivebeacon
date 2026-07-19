# SPDX-License-Identifier: GPL-3.0-only

qt_add_executable(activityparser_test
    activityparser_test.cpp
    ../src/activityparser.cpp
)
target_include_directories(activityparser_test PRIVATE ../src)
target_compile_features(activityparser_test PRIVATE cxx_std_20)
target_link_libraries(activityparser_test PRIVATE Qt6::Core Qt6::Test)

add_test(NAME activityparser_test COMMAND activityparser_test)

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
