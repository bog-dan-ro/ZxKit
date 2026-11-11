# Tools are optional and are never added by a default FetchContent build.
option(ZXKIT_BUILD_MEDIA_TOOL "Build zxmedia" ON)
option(ZXKIT_BUILD_MEDIA_CHECK "Build the optional image read/write audit tool" OFF)
option(ZXKIT_BUILD_FUSE_TOOL "Build zxmount when Linux FUSE3 is available" ON)
option(ZXKIT_BUILD_MC_INTEGRATION "Install Midnight Commander integration" ON)
option(ZXKIT_BUILD_PROFILING_TOOLS "Build the developer profiling tools" OFF)
option(ZXKIT_BUILD_MCP_TOOL "Build zx-mcp (requires C++26 reflection and Boost.JSON)" OFF)
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(ZXKIT_BUILD_MCP_TOOL)
    add_subdirectory(mcp)
endif()

if(ZXKIT_BUILD_MEDIA_TOOL)
    add_executable(zxmedia media/main.cpp)
    target_link_libraries(zxmedia PRIVATE ZX::Kit)
    if(ZXKIT_INSTALL)
        install(TARGETS zxmedia RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(FILES media/zxmedia.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
    endif()
endif()

if(ZXKIT_BUILD_MEDIA_CHECK)
    add_executable(zxmedia-check media/check.cpp)
    target_link_libraries(zxmedia-check PRIVATE ZX::Kit)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ZXKIT_BUILD_FUSE_TOOL)
    find_package(FUSE3 QUIET)
    if(FUSE3_FOUND)
        add_executable(zxmount fuse/main.cpp)
        target_link_libraries(zxmount PRIVATE ZX::Kit FUSE3::FUSE3)
        if(ZXKIT_INSTALL)
            install(TARGETS zxmount RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        endif()
    else()
        message(STATUS "FUSE3 not found: zxmount disabled")
    endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ZXKIT_BUILD_MC_INTEGRATION
        AND TARGET zxmedia AND ZXKIT_INSTALL)
    # MC looks for system-wide helpers in its libexec directory, which is not
    # ours to guess (Debian: /usr/lib/mc/extfs.d), so packagers can point this
    # at it. The default only collects the files; users copy the helper to
    # ~/.local/share/mc/extfs.d or run zxkit-mc-setup for the associations.
    set(ZXKIT_MC_EXTFS_DIR "${CMAKE_INSTALL_DATADIR}/zxkit/mc/extfs.d" CACHE STRING
        "Install directory of the MC extfs helper, relative to the install prefix")
    install(PROGRAMS mc/zxkit DESTINATION ${ZXKIT_MC_EXTFS_DIR})
    install(FILES mc/zxkit.ini DESTINATION ${CMAKE_INSTALL_DATADIR}/zxkit/mc)
    install(PROGRAMS mc/zxkit-mc-setup DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES mc/zxkit-mc-setup.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
endif()

if(ZXKIT_BUILD_PROFILING_TOOLS)
    add_executable(zxprofile profiling/profile_engine.cpp)
    target_link_libraries(zxprofile PRIVATE ZX::VideoFilters)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        include(CheckLanguage)
        check_language(C)
        if(CMAKE_C_COMPILER)
            enable_language(C)
            add_library(zxprofile_allocations MODULE profiling/profile_allocations.c)
            target_link_libraries(zxprofile_allocations PRIVATE ${CMAKE_DL_LIBS})
        endif()
    endif()
endif()
