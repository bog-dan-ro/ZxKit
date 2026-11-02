set(ZXKIT_TEST_ROM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/roms")
add_executable(test_video_filters test_video_filters.cpp)
target_link_libraries(test_video_filters PRIVATE ZX::VideoFilters)
target_compile_options(test_video_filters PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
add_test(NAME test_video_filters COMMAND test_video_filters)

foreach(rom 48.rom 128-0.rom 128-1.rom
        cobra1_boot.rom cobra1_sys-0.rom cobra1_sys-1.rom cobra1_sys-2.rom cobra1_sys-3.rom
        cobra2_ts_boot.rom tc2048.rom tc2068-0.rom tc2068-1.rom se-0.rom se-1.rom)
    if(NOT EXISTS "${ZXKIT_TEST_ROM_DIR}/${rom}")
        message(FATAL_ERROR "Missing bundled test ROM: ${rom}")
    endif()
endforeach()

foreach(media cobra/KRYS80_CL_SYSDISK.img cobra/UTILS1.TD0 teledisk/normal.td0 teledisk/advanced.td0)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/${media}")
        message(FATAL_ERROR "Missing bundled disk fixture: ${media}")
    endif()
endforeach()

add_executable(test_foundation test_foundation.cpp)
target_link_libraries(test_foundation PRIVATE ZX::Kit)
add_test(NAME test_foundation COMMAND test_foundation)

add_executable(test_audio test_audio.cpp)
target_link_libraries(test_audio PRIVATE ZX::Kit)
target_compile_options(test_audio PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
add_test(NAME test_audio COMMAND test_audio)

add_executable(test_fuse_z80 test_fuse_z80.cpp)
target_link_libraries(test_fuse_z80 PRIVATE ZX::Kit)
add_test(NAME test_fuse_z80 COMMAND test_fuse_z80 ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)

add_executable(test_z80n_mmu_machines test_z80n_mmu_machines.cpp)
target_link_libraries(test_z80n_mmu_machines PRIVATE ZX::Kit)
add_test(NAME test_z80n_mmu_machines COMMAND test_z80n_mmu_machines)

# Headless scripted runner (boot checks, screenshots)
add_executable(zxrun tools/zxrun.cpp)
target_link_libraries(zxrun PRIVATE ZX::Kit)
target_compile_definitions(zxrun PRIVATE ZXKIT_TEST_ROM_DIR="${ZXKIT_TEST_ROM_DIR}")

add_executable(test_storage_next test_storage_next.cpp)
target_link_libraries(test_storage_next PRIVATE ZX::Kit ZLIB::ZLIB)
add_test(NAME test_storage_next COMMAND test_storage_next)

add_executable(test_tape test_tape.cpp)
target_link_libraries(test_tape PRIVATE ZX::Kit)
target_compile_definitions(test_tape PRIVATE ZXKIT_TEST_ROM_DIR="${ZXKIT_TEST_ROM_DIR}")
# ROM fixtures are part of ZxKit; no application checkout is required.
add_test(NAME test_tape COMMAND test_tape ${ZXKIT_TEST_ROM_DIR})

add_executable(test_disks test_disks.cpp)
target_link_libraries(test_disks PRIVATE ZX::Kit)
add_test(NAME test_disks COMMAND test_disks)

# The CoBra and Timex tests boot the ROMs (and the CoBra's CP/M disk) from tests/fixtures
add_executable(test_cobra test_cobra.cpp)
target_link_libraries(test_cobra PRIVATE ZX::Kit)
target_compile_definitions(test_cobra PRIVATE ZXKIT_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_test(NAME test_cobra COMMAND test_cobra ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)

add_executable(test_timex test_timex.cpp)
target_link_libraries(test_timex PRIVATE ZX::Kit)
target_compile_definitions(test_timex PRIVATE ZXKIT_TEST_ROM_DIR="${ZXKIT_TEST_ROM_DIR}")
add_test(NAME test_timex COMMAND test_timex ${ZXKIT_TEST_ROM_DIR})

add_executable(test_teledisk test_teledisk.cpp)
target_link_libraries(test_teledisk PRIVATE ZX::Kit)
target_compile_definitions(test_teledisk PRIVATE ZXKIT_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_test(NAME test_teledisk COMMAND test_teledisk ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)

# The tests check with assert(): keep it in Release builds too
foreach(test_target test_foundation test_fuse_z80 test_z80n_mmu_machines test_storage_next test_tape test_disks test_cobra test_timex test_teledisk)
    target_compile_options(${test_target} PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
endforeach()

add_executable(test_spectrum_timing test_spectrum_timing.cpp)
target_link_libraries(test_spectrum_timing PRIVATE ZX::Kit)
target_compile_options(test_spectrum_timing PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
add_test(NAME spectrum_timing_boundaries COMMAND test_spectrum_timing)
add_test(NAME spectrum_48k_suites COMMAND test_spectrum_timing
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/spectrum
    ${ZXKIT_TEST_ROM_DIR} 48k)
add_test(NAME spectrum_128k_suites COMMAND test_spectrum_timing
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/spectrum
    ${ZXKIT_TEST_ROM_DIR} 128k)


add_executable(test_ula_raster test_ula_raster.cpp)
target_link_libraries(test_ula_raster PRIVATE ZX::Kit)
target_compile_options(test_ula_raster PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
add_test(NAME spectrum_ula_raster COMMAND test_ula_raster)

add_executable(test_borders test_borders.cpp)
target_link_libraries(test_borders PRIVATE ZX::Kit)
target_compile_options(test_borders PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
add_test(NAME test_borders COMMAND test_borders)

add_test(NAME spectrum_48k_basic_border COMMAND test_ula_raster
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/spectrum ${ZXKIT_TEST_ROM_DIR})

add_executable(test_next_devices test_next_devices.cpp)
target_link_libraries(test_next_devices PRIVATE ZX::Kit)
target_compile_options(test_next_devices PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
add_test(NAME test_next_devices COMMAND test_next_devices)
