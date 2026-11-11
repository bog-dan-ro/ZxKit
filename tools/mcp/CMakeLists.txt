cmake_minimum_required(VERSION 3.30)

find_package(Boost 1.92 REQUIRED COMPONENTS json beast asio)
find_package(OpenSSL 3 REQUIRED)
include(FetchContent)
FetchContent_Declare(MCPpp
    GIT_REPOSITORY https://github.com/bog-dan-ro/MCPpp.git
    GIT_TAG 1b1848860d0a7387b4611e53a8e62ac5efc6b4bd)
FetchContent_MakeAvailable(MCPpp)

add_executable(zx-mcp main.cpp Tools.cpp Support.cpp Roms.cpp BasicListing.cpp Download.cpp Next.cpp)
target_link_libraries(zx-mcp PRIVATE ZX::Kit mcp::mcpp Boost::json Boost::beast Boost::asio OpenSSL::SSL OpenSSL::Crypto)
target_compile_features(zx-mcp PRIVATE cxx_std_26)

if(ZXKIT_INSTALL)
    install(TARGETS zx-mcp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/zxkit/zx-mcp)
    install(DIRECTORY skills/zx-mcp DESTINATION ${CMAKE_INSTALL_DATADIR}/zxkit/zx-mcp/skills
        PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE)
    install(FILES ${PROJECT_SOURCE_DIR}/tests/fixtures/roms/README.copyright
        DESTINATION ${CMAKE_INSTALL_DATADIR}/zxkit/zx-mcp RENAME ROM-NOTICES)
endif()
