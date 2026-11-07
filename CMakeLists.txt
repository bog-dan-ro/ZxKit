cmake_minimum_required(VERSION 3.25)

project(zxkit
    VERSION 1.0.0
    DESCRIPTION "Modern C++23/26 ZX Spectrum, Z80 & ZX Spectrum Next Emulation Framework"
    LANGUAGES CXX)

include(GNUInstallDirs)

# A consumer that pulls zxkit in with FetchContent must not end up building our
# tests or our install rules by default
set(ZXKIT_IS_TOP_LEVEL OFF)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(ZXKIT_IS_TOP_LEVEL ON)
endif()

option(ZXKIT_BUILD_TESTS "Build the zxkit unit tests" ${ZXKIT_IS_TOP_LEVEL})
option(ZXKIT_INSTALL "Generate the install and package rules" ${ZXKIT_IS_TOP_LEVEL})
option(ZXKIT_BUILD_TOOLS "Build optional ZxKit command-line tools" ${ZXKIT_IS_TOP_LEVEL})

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

# SafeCpp: thread safe smart pointers, every shared object is reached through a lock guard
if(NOT TARGET Safe::CPP)
    find_package(SafeCpp 1.0 QUIET)
endif()
if(NOT TARGET Safe::CPP)
    include(FetchContent)
    # Installing zxkit exports Safe::CPP as a dependency, so SafeCpp must install too
    set(SAFECPP_INSTALL ${ZXKIT_INSTALL} CACHE BOOL "" FORCE)
    FetchContent_Declare(SafeCpp
        GIT_REPOSITORY https://github.com/bog-dan-ro/safecpp.git
        GIT_TAG v1.0)
    FetchContent_MakeAvailable(SafeCpp)
endif()

# ---------------------------------------------------------------- the library

set(ZXKIT_PUBLIC_HEADERS
    include/zxkit/zxkit.h
    include/zxkit/core/Clock.h
    include/zxkit/core/Emulator.h
    include/zxkit/core/Engine.h
    include/zxkit/core/Events.h
    include/zxkit/core/Exception.h
    include/zxkit/core/OptionalRef.h
    include/zxkit/core/Scheduler.h
    include/zxkit/core/Shared.h
    include/zxkit/core/SpinSharedMutex.h
    include/zxkit/core/StateStream.h
    include/zxkit/cpu/Im2Controller.h
    include/zxkit/cpu/Z80Ctc.h
    include/zxkit/cpu/Z80Cpu.h
    include/zxkit/cpu/Z80Registers.h
    include/zxkit/memory/ContentionModel.h
    include/zxkit/memory/MemoryBank.h
    include/zxkit/memory/Mmu.h
    include/zxkit/bus/IoBus.h
    include/zxkit/bus/Peripheral.h
    include/zxkit/bus/RomcsStack.h
    include/zxkit/video/UlaRenderer.h
    include/zxkit/video/VideoFrame.h
    include/zxkit/video/VideoFilters.h
    include/zxkit/video/next/NextVideo.h
    include/zxkit/audio/AudioMixer.h
    include/zxkit/audio/Ay8912.h
    include/zxkit/audio/Beeper.h
    include/zxkit/audio/BlipBuffer.h
    include/zxkit/audio/SpeakerFilter.h
    include/zxkit/audio/Dac.h
    include/zxkit/input/Joystick.h
    include/zxkit/input/KeyboardMatrix.h
    include/zxkit/input/Mouse.h
    include/zxkit/peripherals/expansion/ExpansionPeripherals.h
    include/zxkit/peripherals/net/NetworkPeripherals.h
    include/zxkit/peripherals/next/Copper.h
    include/zxkit/peripherals/next/NextUart.h
    include/zxkit/peripherals/next/NextRtc.h
    include/zxkit/peripherals/next/NextReg.h
    include/zxkit/peripherals/next/ZxnCtc.h
    include/zxkit/peripherals/next/ZxnDma.h
    include/zxkit/peripherals/storage/FloppyDisk.h
    include/zxkit/peripherals/storage/Interface1.h
    include/zxkit/peripherals/storage/Beta128.h
    include/zxkit/peripherals/storage/FatVolume.h
    include/zxkit/peripherals/storage/MassStorage.h
    include/zxkit/peripherals/storage/SdCard.h
    include/zxkit/peripherals/storage/Wd1793.h
    include/zxkit/machines/CloneMachines.h
    include/zxkit/machines/MachineProfile.h
    include/zxkit/machines/RomanianMachines.h
    include/zxkit/machines/SinclairMachines.h
    include/zxkit/machines/ZxNextMachine.h
    include/zxkit/media/MediaArchive.h
    include/zxkit/media/BasicCodec.h
    include/zxkit/media/TapeConversion.h
    include/zxkit/media/Compression.h
    include/zxkit/media/MediaPreview.h
    include/zxkit/media/MediaRegistry.h
    include/zxkit/media/RzxFormat.h
    include/zxkit/media/ScreenshotFormat.h
    include/zxkit/media/SnapshotFormats.h
    include/zxkit/media/TapeDeck.h
    include/zxkit/media/TapeFormats.h
    include/zxkit/debug/Assembler.h
    include/zxkit/debug/Breakpoints.h
    include/zxkit/debug/Debugger.h
    include/zxkit/debug/Disassembler.h
    include/zxkit/debug/PokeFinder.h
    include/zxkit/debug/RewindBuffer.h
    include/zxkit/debug/SymbolTable.h
)

set(ZXKIT_SOURCES
    src/core/Clock.cpp
    src/core/Emulator.cpp
    src/core/Engine.cpp
    src/core/Scheduler.cpp
    src/core/StateStream.cpp
    src/cpu/Im2Controller.cpp
    src/cpu/Z80Ctc.cpp
    src/cpu/Z80Cpu.cpp
    src/cpu/Z80Registers.cpp
    src/memory/ContentionModel.cpp
    src/memory/MemoryBank.cpp
    src/memory/Mmu.cpp
    src/bus/IoBus.cpp
    src/bus/Peripheral.cpp
    src/bus/RomcsStack.cpp
    src/video/UlaRenderer.cpp
    src/video/VideoFrame.cpp
    src/video/next/NextVideo.cpp
    src/audio/AudioMixer.cpp
    src/audio/Ay8912.cpp
    src/audio/Beeper.cpp
    src/audio/BlipBuffer.cpp
    src/audio/SpeakerFilter.cpp
    src/audio/Dac.cpp
    src/input/Joystick.cpp
    src/input/KeyboardMatrix.cpp
    src/input/Mouse.cpp
    src/peripherals/expansion/ExpansionPeripherals.cpp
    src/peripherals/net/NetworkPeripherals.cpp
    src/peripherals/next/Copper.cpp
    src/peripherals/next/NextUart.cpp
    src/peripherals/next/NextRtc.cpp
    src/peripherals/next/NextReg.cpp
    src/peripherals/next/ZxnCtc.cpp
    src/peripherals/next/ZxnDma.cpp
    src/peripherals/storage/Beta128.cpp
    src/peripherals/storage/FloppyDisk.cpp
    src/peripherals/storage/Teledisk.cpp
    src/peripherals/storage/Interface1.cpp
    src/peripherals/storage/FatVolume.cpp
    src/peripherals/storage/MassStorage.cpp
    src/peripherals/storage/SdCard.cpp
    src/peripherals/storage/Wd1793.cpp
    src/machines/CloneMachines.cpp
    src/machines/MachineProfile.cpp
    src/machines/MachineRegistry.cpp
    src/machines/RomanianMachines.cpp
    src/machines/SinclairMachines.cpp
    src/machines/ZxNextMachine.cpp
    src/media/MediaArchive.cpp
    src/media/BasicCodec.cpp
    src/media/TapeConversion.cpp
    src/media/archive/TapeArchive.cpp
    src/media/archive/NexArchive.cpp
    src/media/archive/MicrodriveArchive.cpp
    src/media/archive/DiskContainer.cpp
    src/media/archive/MdosArchive.cpp
    src/media/archive/DiskArchive.cpp
    src/media/archive/FatArchive.cpp
    src/media/Compression.cpp
    src/media/MediaPreview.cpp
    src/media/MediaRegistry.cpp
    src/media/RzxFormat.cpp
    src/media/ScreenshotFormat.cpp
    src/media/SnapshotFormats.cpp
    src/media/TapeDeck.cpp
    src/media/TapeFormats.cpp
    src/debug/Assembler.cpp
    src/debug/Breakpoints.cpp
    src/debug/Debugger.cpp
    src/debug/Disassembler.cpp
    src/debug/PokeFinder.cpp
    src/debug/RewindBuffer.cpp
    src/debug/SymbolTable.cpp
)

add_library(zxkit ${ZXKIT_SOURCES} ${ZXKIT_PUBLIC_HEADERS})
add_library(ZX::Kit ALIAS zxkit)
add_library(zxkit::zxkit ALIAS zxkit)

target_compile_features(zxkit PUBLIC cxx_std_23)

# NDK r27's libc++ implements jthread/stop_token behind its experimental-library
# switch. Probe the toolchain rather than requiring that flag on newer NDKs.
# Engine's public header uses jthread, so consumers need the same configuration.
if(ANDROID)
    # Qt's targets already do this; standalone/shared ZxKit targets and their
    # consumers must also load on Android devices with 16 KiB memory pages.
    target_link_options(zxkit PUBLIC "LINKER:-z,max-page-size=16384")
    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_FLAGS "-std=c++23")
    set(_zxkit_thread_probe "#include <thread>\nint main() { std::jthread t([](std::stop_token) {}); }")
    check_cxx_source_compiles("${_zxkit_thread_probe}" ZXKIT_HAS_JTHREAD)
    if(NOT ZXKIT_HAS_JTHREAD)
        set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -fexperimental-library")
        check_cxx_source_compiles("${_zxkit_thread_probe}" ZXKIT_HAS_EXPERIMENTAL_JTHREAD)
        if(NOT ZXKIT_HAS_EXPERIMENTAL_JTHREAD)
            message(FATAL_ERROR "The Android C++ library must provide std::jthread and std::stop_token.")
        endif()
        target_compile_options(zxkit PUBLIC -fexperimental-library)
        target_link_options(zxkit PUBLIC -fexperimental-library)
    endif()
    cmake_pop_check_state()
    unset(_zxkit_thread_probe)
endif()

target_include_directories(zxkit
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(zxkit
    PUBLIC
        Safe::CPP
        Threads::Threads
    PRIVATE
        ZLIB::ZLIB
)

set_target_properties(zxkit PROPERTIES EXPORT_NAME Kit)

# Optional presentation library: the emulator and hardware-rendering consumers
# never depend on it. All algorithm sources are bundled; no extra downloads.
add_library(zxkit_video_filters
    src/video/filters/VideoFilters.cpp
    src/video/filters/fuse/FuseScalers.cpp
    src/video/filters/xbrz/xbrz.cpp
)
add_library(ZX::VideoFilters ALIAS zxkit_video_filters)
target_link_libraries(zxkit_video_filters PUBLIC ZX::Kit)
set_target_properties(zxkit_video_filters PROPERTIES EXPORT_NAME VideoFilters)

# ----------------------------------------------------------------- tests

if(ZXKIT_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(ZXKIT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ----------------------------------------------------------------- install

if(ZXKIT_INSTALL)
    include(CMakePackageConfigHelpers)

    install(TARGETS zxkit
        EXPORT zxkitTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    install(TARGETS zxkit_video_filters
        EXPORT zxkitTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(FILES src/video/filters/README.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/zxkit/video-filters)
    install(FILES src/video/filters/xbrz/License.txt
        DESTINATION ${CMAKE_INSTALL_DATADIR}/zxkit/video-filters/xbrz)
    install(FILES LICENSES/ayumi.txt
        DESTINATION ${CMAKE_INSTALL_DATADIR}/zxkit/licenses)

    install(DIRECTORY include/zxkit
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    install(EXPORT zxkitTargets
        FILE zxkitTargets.cmake
        NAMESPACE ZX::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zxkit
    )

    configure_package_config_file(cmake/zxkitConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/zxkitConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zxkit
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/zxkitConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/zxkitConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/zxkitConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/zxkit
    )
endif()
