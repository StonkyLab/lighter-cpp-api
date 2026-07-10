cmake_minimum_required(VERSION 4.0)
project(lighter_api)

set(CMAKE_CXX_STANDARD 20)

if (MSVC)
    add_definitions(-D_WIN32_WINNT=0x0A00 /bigobj /utf-8 -DVERBOSE_LOG)
else ()
    add_definitions(-fPIC)
endif ()

if (POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif ()

find_package(Boost 1.88 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(magic_enum REQUIRED)

include_directories(include stonky-cpp-common/include SYSTEM ${Boost_INCLUDE_DIR} ${OPENSSL_INCLUDE_DIR})

if (NOT TARGET stonky_common)
    add_subdirectory(stonky-cpp-common)
endif ()

set(HEADERS
        include/stonky/lighter/lighter.h
        include/stonky/lighter/lighter_enums.h
        include/stonky/lighter/lighter_models.h
        include/stonky/lighter/lighter_http_session.h
        include/stonky/lighter/lighter_rest_client.h
        include/stonky/lighter/lighter_signer.h
        include/stonky/lighter/lighter_ws_session.h
        include/stonky/lighter/lighter_ws_stream_manager.h)

set(SOURCES
        src/lighter.cpp
        src/lighter_models.cpp
        src/lighter_http_session.cpp
        src/lighter_rest_client.cpp
        src/lighter_signer.cpp
        src/lighter_ws_session.cpp
        src/lighter_ws_stream_manager.cpp)

option(ENABLE_TESTS "Build lighter_api test executables" OFF)

# Lighter's prebuilt native signer (Go c-shared lib) — orders are zk-rollup L2
# txs signed by a scheme not reproducible with stdlib crypto, so we link the
# vendored .so exactly as the reference Python/TS SDKs FFI into it. It ships only
# as a Linux .so (no Windows build), so it is wired up only on non-Windows. The
# data downloader consumes the STATIC lib and never references the signer object,
# so its link never pulls the .so in.
if (NOT WIN32)
    set(LIGHTER_SIGNER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lighter-signer CACHE INTERNAL "Lighter native signer dir")

    if (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(LIGHTER_SIGNER_LIB ${LIGHTER_SIGNER_DIR}/lighter-signer-linux-arm64.so CACHE INTERNAL "Lighter native signer lib")
    else ()
        set(LIGHTER_SIGNER_LIB ${LIGHTER_SIGNER_DIR}/lighter-signer-linux-amd64.so CACHE INTERNAL "Lighter native signer lib")
    endif ()

    # IMPORTED target so CMake links the signer by its full path (the file has no
    # `lib` prefix, so a bare `-l` search cannot find it).
    if (NOT TARGET lighter_signer)
        add_library(lighter_signer SHARED IMPORTED GLOBAL)
        set_target_properties(lighter_signer PROPERTIES IMPORTED_LOCATION ${LIGHTER_SIGNER_LIB})
    endif ()
endif ()

if (MODULE_MANAGER)
    add_library(lighter_api SHARED ${SOURCES} ${HEADERS})
    # The SHARED module bakes in lighter_signer.cpp; its native symbols come from
    # the signer .so, so link it here (PRIVATE — resolved inside the module).
    if (TARGET lighter_signer)
        target_link_libraries(lighter_api PRIVATE lighter_signer)
    endif ()
else ()
    add_library(lighter_api STATIC ${SOURCES} ${HEADERS})
endif ()

target_link_libraries(lighter_api PRIVATE spdlog::spdlog_header_only OpenSSL::Crypto OpenSSL::SSL stonky_common nlohmann_json::nlohmann_json)

if (NOT MSVC)
    target_link_libraries(lighter_api PRIVATE atomic)
endif ()

if (ENABLE_TESTS)
    add_executable(lighter_test test/main.cpp)
    target_link_libraries(lighter_test PRIVATE spdlog::spdlog_header_only lighter_api OpenSSL::Crypto)

    add_executable(lighter_signer_smoke test/signer_smoke.cpp)
    target_link_libraries(lighter_signer_smoke PRIVATE spdlog::spdlog_header_only lighter_api OpenSSL::Crypto)

    add_executable(lighter_market_probe test/market_probe.cpp)
    target_link_libraries(lighter_market_probe PRIVATE spdlog::spdlog_header_only lighter_api OpenSSL::Crypto)

    # The tests link lighter_api STATIC and pull in lighter_signer.cpp, so they
    # need the native signer .so and its dir on the build-tree rpath.
    if (TARGET lighter_signer)
        target_link_libraries(lighter_test PRIVATE lighter_signer)
        target_link_libraries(lighter_signer_smoke PRIVATE lighter_signer)
        target_link_libraries(lighter_market_probe PRIVATE lighter_signer)
        set_target_properties(lighter_test lighter_signer_smoke lighter_market_probe
                PROPERTIES BUILD_RPATH ${LIGHTER_SIGNER_DIR})
    endif ()
endif ()
