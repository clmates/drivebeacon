qt_add_executable(activityparser_test
    activityparser_test.cpp
    ../src/activityparser.cpp
)
target_include_directories(activityparser_test PRIVATE ../src)
target_compile_features(activityparser_test PRIVATE cxx_std_20)
target_link_libraries(activityparser_test PRIVATE Qt6::Core Qt6::Test)

add_test(NAME activityparser_test COMMAND activityparser_test)
