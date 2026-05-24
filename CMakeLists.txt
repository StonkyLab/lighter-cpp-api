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
        include/stonky/lighter/lighter_rest_client.h)

set(SOURCES
        src/lighter.cpp
        src/lighter_models.cpp
        src/lighter_http_session.cpp
        src/lighter_rest_client.cpp)

if (MODULE_MANAGER)
    add_library(lighter_api SHARED ${SOURCES} ${HEADERS})
else ()
    add_library(lighter_api STATIC ${SOURCES} ${HEADERS})

    add_executable(test test/main.cpp)
    target_link_libraries(test PRIVATE spdlog::spdlog_header_only lighter_api OpenSSL::Crypto)

endif ()

target_link_libraries(lighter_api PRIVATE spdlog::spdlog_header_only OpenSSL::Crypto OpenSSL::SSL stonky_common nlohmann_json::nlohmann_json)

if (NOT MSVC)
    target_link_libraries(lighter_api PRIVATE atomic)
endif ()
