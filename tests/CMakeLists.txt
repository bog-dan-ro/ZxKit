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

add_executable(test_storage_next test_storage_next.cpp)
target_link_libraries(test_storage_next PRIVATE ZX::Kit ZLIB::ZLIB)
add_test(NAME test_storage_next COMMAND test_storage_next)

add_executable(test_tape test_tape.cpp)
target_link_libraries(test_tape PRIVATE ZX::Kit)
# The ROM loading tests run when Spectacol (and its ROMs) is checked out
add_test(NAME test_tape COMMAND test_tape ${CMAKE_CURRENT_SOURCE_DIR}/spectacol/spectacol/roms)

add_executable(test_disks test_disks.cpp)
target_link_libraries(test_disks PRIVATE ZX::Kit)
add_test(NAME test_disks COMMAND test_disks)

# The tests check with assert(): keep it in Release builds too
foreach(test_target test_foundation test_fuse_z80 test_z80n_mmu_machines test_storage_next test_tape test_disks)
    target_compile_options(${test_target} PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
endforeach()
