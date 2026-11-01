set(ZXKIT_TEST_ROM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/roms")
foreach(rom 48.rom 128-0.rom 128-1.rom)
    if(NOT EXISTS "${ZXKIT_TEST_ROM_DIR}/${rom}")
        message(FATAL_ERROR "Missing bundled test ROM: ${rom}")
    endif()
endforeach()

add_executable(test_foundation test_foundation.cpp)
target_link_libraries(test_foundation PRIVATE ZX::Kit)
add_test(NAME test_foundation COMMAND test_foundation)

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

# The tests check with assert(): keep it in Release builds too
foreach(test_target test_foundation test_fuse_z80 test_z80n_mmu_machines test_storage_next test_tape test_disks)
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

add_test(NAME spectrum_48k_basic_border COMMAND test_ula_raster
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/spectrum ${ZXKIT_TEST_ROM_DIR})
