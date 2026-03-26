cmake_minimum_required(VERSION 3.20)

project(ThinkboxLibraryTest LANGUAGES CXX)

add_executable(test_thinkboxlibrary)

target_compile_features(test_thinkboxlibrary PRIVATE cxx_std_17)

set_target_properties(test_thinkboxlibrary PROPERTIES
    CXX_EXTENSIONS OFF
)

if(MSVC)
    target_compile_options(test_thinkboxlibrary PRIVATE /W4 /WX /MP)
    target_compile_options(test_thinkboxlibrary PRIVATE
        "$<$<CONFIG:Release>:/O2>"
        "$<$<CONFIG:RelWithDebInfo>:/Od>"
    )
    target_compile_definitions(test_thinkboxlibrary PRIVATE
        _USE_MATH_DEFINES
        _UNICODE
        _CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_options(test_thinkboxlibrary PRIVATE -Wall -Wextra -Wpedantic -Werror -pthread)
endif()

if(UNIX)
    target_compile_definitions(test_thinkboxlibrary PRIVATE
        _BOOL
        REQUIRE_IOSTREAM
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(test_thinkboxlibrary PRIVATE
        LINUX=1
        BOOST_FILESYSTEM_VERSION=3
    )
endif()

file(GLOB_RECURSE TEST_THINKBOXLIBRARY_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)

file(GLOB_RECURSE TEST_THINKBOXLIBRARY_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
)

target_sources(test_thinkboxlibrary
    PRIVATE
        stdafx.cpp
        stdafx.h
        ${TEST_THINKBOXLIBRARY_HEADERS}
        ${TEST_THINKBOXLIBRARY_SOURCES}
)

target_include_directories(test_thinkboxlibrary
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(test_thinkboxlibrary
    PRIVATE
        thinkboxlibrary
        GTest::gtest
        GTest::gtest_main
)

target_precompile_headers(test_thinkboxlibrary PRIVATE stdafx.h)

include(GoogleTest)
gtest_add_tests(TARGET test_thinkboxlibrary)
