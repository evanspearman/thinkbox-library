cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(
    ThinkboxLibrary
    VERSION 0.1.0
    DESCRIPTION "Thinkbox core C++ library"
    LANGUAGES CXX
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)
include(FindPkgConfig)

option(BUILD_UNIT_TESTS "Build unit tests" OFF)
option(BUILD_WITH_TBB "Build with Intel Thread Building Blocks" ON)
option(BUILD_WITH_E57 "Build with Xerces and libE57 to enable E57 file IO" OFF)
option(BUILD_WITH_LZ4 "Build with LZ4 to enable PRT2 file format" OFF)

add_library(thinkboxlibrary STATIC)
add_library(Thinkbox::thinkboxlibrary ALIAS thinkboxlibrary)

target_compile_features(thinkboxlibrary PUBLIC cxx_std_17)

set_target_properties(thinkboxlibrary PROPERTIES
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
    EXPORT_NAME thinkboxlibrary
)

file(GLOB_RECURSE THINKBOXLIBRARY_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/frantic/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/frantic/*.hpp"
)

file(GLOB_RECURSE THINKBOXLIBRARY_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

target_sources(thinkboxlibrary
    PRIVATE
        stdafx.cpp
        stdafx.h
        ${THINKBOXLIBRARY_HEADERS}
        ${THINKBOXLIBRARY_SOURCES}
)

target_include_directories(thinkboxlibrary
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(MSVC)
    target_compile_options(thinkboxlibrary PRIVATE /W4 /WX /MP)
    target_compile_options(thinkboxlibrary PUBLIC /wd4503)

    target_compile_options(thinkboxlibrary PRIVATE
        "$<$<CONFIG:Release>:/O2>"
        "$<$<CONFIG:RelWithDebInfo>:/Od>"
    )

    target_compile_definitions(thinkboxlibrary PRIVATE
        _USE_MATH_DEFINES
        _UNICODE
        _CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_options(thinkboxlibrary PRIVATE -Wall -Wextra -Wpedantic -Werror -pthread)

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(thinkboxlibrary PUBLIC
            -Wno-deprecated-declarations
            -Wno-strict-overflow
            -Wno-unused-variable
        )
    endif()
endif()

if(UNIX)
    target_compile_definitions(thinkboxlibrary PRIVATE
        _BOOL
        REQUIRE_IOSTREAM
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(thinkboxlibrary PRIVATE
        LINUX=1
        BOOST_FILESYSTEM_VERSION=3
    )
endif()

target_precompile_headers(thinkboxlibrary PRIVATE stdafx.h)

pkg_check_modules(LIBB2 REQUIRED IMPORTED_TARGET libb2)
find_package(Boost REQUIRED)
find_package(BZip2 REQUIRED)
find_package(Eigen3 REQUIRED CONFIG)
find_package(glog REQUIRED)
find_package(OpenEXR REQUIRED)
find_package(ZLIB REQUIRED)
find_package(tinyxml2 REQUIRED CONFIG)
find_package(utf8cpp REQUIRED)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)

target_link_libraries(thinkboxlibrary
    PUBLIC
        PkgConfig::LIBB2
        Boost::boost
        BZip2::BZip2
        Eigen3::Eigen
        glog::glog
        OpenEXR::OpenEXR
        ZLIB::ZLIB
        tinyxml2::tinyxml2
        utf8cpp::utf8cpp
        PkgConfig::XXHASH
)

if(BUILD_WITH_TBB)
    find_package(TBB REQUIRED)
    target_link_libraries(thinkboxlibrary PUBLIC TBB::tbb)
    target_compile_definitions(thinkboxlibrary PUBLIC FRANTIC_TBB_AVAILABLE)
else()
    target_compile_definitions(thinkboxlibrary PUBLIC FRANTIC_DISABLE_THREADS)
endif()

if(UNIX AND NOT APPLE)
    find_package(ICU REQUIRED COMPONENTS uc i18n)
    target_link_libraries(thinkboxlibrary PUBLIC ICU::uc ICU::i18n)
endif()

if(BUILD_WITH_E57)
    pkg_check_modules(XERCESC REQUIRED IMPORTED_TARGET xerces-c)
    pkg_check_modules(E57FORMAT REQUIRED IMPORTED_TARGET libe57format)

    target_link_libraries(thinkboxlibrary
        PUBLIC
            PkgConfig::XERCESC
            PkgConfig::E57FORMAT
    )

    target_compile_definitions(thinkboxlibrary PUBLIC E57_AVAILABLE)
endif()

if(BUILD_WITH_LZ4)
    find_package(lz4 REQUIRED)
    target_link_libraries(thinkboxlibrary PUBLIC lz4::lz4)
    target_compile_definitions(thinkboxlibrary PUBLIC LZ4_AVAILABLE)
endif()

target_compile_definitions(thinkboxlibrary
    PUBLIC
        BOOST_AUTO_LINK_SYSTEM
        _SILENCE_CXX17_OLD_ALLOCATOR_MEMBERS_DEPRECATION_WARNING
        EIGEN_MPL2_ONLY
)

if(BUILD_UNIT_TESTS)
    add_subdirectory(UnitTests)
endif()

install(
    TARGETS thinkboxlibrary
    EXPORT ThinkboxLibraryTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(
    DIRECTORY frantic/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/frantic
    FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.hpp"
)

install(
    FILES stdafx.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
    EXPORT ThinkboxLibraryTargets
    FILE ThinkboxLibraryTargets.cmake
    NAMESPACE Thinkbox::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ThinkboxLibrary
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ThinkboxLibraryConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ThinkboxLibraryConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ThinkboxLibrary"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ThinkboxLibraryConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/ThinkboxLibraryConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/ThinkboxLibraryConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ThinkboxLibrary"
)
