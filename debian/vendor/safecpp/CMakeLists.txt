cmake_minimum_required(VERSION 3.16)

project(SafeCpp
    VERSION 1.0.0
    DESCRIPTION "Thread safe smart pointers for C++23"
    LANGUAGES CXX)

include(GNUInstallDirs)

# A consumer that pulls SafeCpp in with FetchContent must not end up building our
# demo, our tests, or our install rules
set(SAFECPP_IS_TOP_LEVEL OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(SAFECPP_IS_TOP_LEVEL ON)
endif()

option(SAFECPP_BUILD_EXAMPLES "Build the examples" ${SAFECPP_IS_TOP_LEVEL})
option(SAFECPP_BUILD_TESTS "Build the unit tests" ${SAFECPP_IS_TOP_LEVEL})
option(SAFECPP_INSTALL "Generate the install and package rules" ${SAFECPP_IS_TOP_LEVEL})

# ---------------------------------------------------------------- the library

add_library(SafeCpp INTERFACE)
add_library(Safe::CPP ALIAS SafeCpp)

# C++23 is the public baseline for the library and all consumers.
target_compile_features(SafeCpp INTERFACE cxx_std_23)

target_include_directories(SafeCpp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# Exported as Safe::CPP, otherwise the namespace would give us Safe::SafeCpp
set_target_properties(SafeCpp PROPERTIES EXPORT_NAME CPP)

# An INTERFACE library has no sources of its own, so IDEs would not show the
# header anywhere. This target exists purely to put it in the project tree, it
# builds nothing and is not created for projects that consume us.
if(SAFECPP_IS_TOP_LEVEL)
    add_custom_target(SafeCppHeader SOURCES SafeCpp.h)
endif()

# ----------------------------------------------------------------- examples

if(SAFECPP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ----------------------------------------------------------------- tests

if(SAFECPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ----------------------------------------------------------------- install

if(SAFECPP_INSTALL)
    include(CMakePackageConfigHelpers)

    install(TARGETS SafeCpp EXPORT SafeCppTargets)
    install(FILES SafeCpp.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT SafeCppTargets
        FILE SafeCppTargets.cmake
        NAMESPACE Safe::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SafeCpp)

    configure_package_config_file(cmake/SafeCppConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/SafeCppConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SafeCpp)

    # Header only, so the package is valid for any architecture
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/SafeCppConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
        ARCH_INDEPENDENT)

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/SafeCppConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/SafeCppConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SafeCpp)
endif()
