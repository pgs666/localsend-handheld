cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "VitaSDK toolchain")
  else()
    message(FATAL_ERROR "Set VITASDK or pass CMAKE_TOOLCHAIN_FILE")
  endif()
endif()

project(localsend_handheld_psv C CXX)

include("${VITASDK}/share/vita.cmake" REQUIRED)

set(PROJECT_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

set(VITA_APP_NAME "LocalSend Handheld")
set(VITA_TITLEID "LSHD00001")
set(VITA_VERSION "01.00")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")
set(PSV_ASSETS_FILES
  assets/icon0.png sce_sys/icon0.png
  ${CMAKE_CURRENT_LIST_DIR}/../../third_party/borealis/resources resources
)

set(PLATFORM_PSV ON CACHE BOOL "" FORCE)
set(USE_GXM ON CACHE BOOL "" FORCE)
set(BOREALIS_USE_GXM ON CACHE BOOL "" FORCE)
set(USE_LIBROMFS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(USE_SYSTEM_FMT OFF CACHE BOOL "" FORCE)
set(USE_SYSTEM_TINYXML2 OFF CACHE BOOL "" FORCE)
set(USE_SYSTEM_TWEENY OFF CACHE BOOL "" FORCE)
set(BRLS_UNITY_BUILD OFF CACHE BOOL "" FORCE)
set(BRLS_RESOURCES_DIR "app0:" CACHE STRING "" FORCE)

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_USER_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/src/mbedtls_vita_config.h" CACHE FILEPATH "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
add_subdirectory(${PROJECT_ROOT}/third_party/mbedtls
  ${CMAKE_CURRENT_BINARY_DIR}/mbedtls EXCLUDE_FROM_ALL)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../third_party/borealis/library
  ${CMAKE_CURRENT_BINARY_DIR}/borealis)

add_library(localsend-core-psv STATIC
  ${PROJECT_ROOT}/src/core/app_service.cpp
  ${PROJECT_ROOT}/src/core/config.cpp
  ${PROJECT_ROOT}/src/core/device_store.cpp
  ${PROJECT_ROOT}/src/core/discovery.cpp
  ${PROJECT_ROOT}/src/core/file_browser.cpp
  ${PROJECT_ROOT}/src/core/http.cpp
  ${PROJECT_ROOT}/src/core/json.cpp
  ${PROJECT_ROOT}/src/core/protocol.cpp
  ${PROJECT_ROOT}/src/core/safe_path.cpp
  ${PROJECT_ROOT}/src/core/security.cpp
  ${PROJECT_ROOT}/src/core/tls.cpp
  ${PROJECT_ROOT}/src/core/transfer.cpp
)

set_target_properties(localsend-core-psv PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
)

target_include_directories(localsend-core-psv PUBLIC
  ${PROJECT_ROOT}/include
)

target_compile_definitions(localsend-core-psv PUBLIC
  LOCALSEND_HAS_MBEDTLS=1
  LOCALSEND_PLATFORM_PSV=1
)

target_compile_options(localsend-core-psv PRIVATE
  -ffunction-sections
  -fdata-sections
)

target_link_libraries(localsend-core-psv PUBLIC
  mbedtls
  mbedx509
  mbedcrypto
  -Wl,--whole-archive -lpthread -Wl,--no-whole-archive
)

add_executable(localsend-core-psv-smoke
  src/core_smoke.cpp
)

set_target_properties(localsend-core-psv-smoke PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
)

target_link_libraries(localsend-core-psv-smoke PRIVATE
  -Wl,--whole-archive localsend-core-psv -Wl,--no-whole-archive
  mbedtls
  mbedx509
  mbedcrypto
)

add_executable(localsend-handheld-psv
  src/main.cpp
)

set_target_properties(localsend-handheld-psv PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
)

target_compile_definitions(localsend-handheld-psv PRIVATE
  __PSV__
  __psp2__
)

target_compile_options(localsend-handheld-psv PRIVATE
  -ffunction-sections
  -fdata-sections
)

target_link_libraries(localsend-handheld-psv PRIVATE
  borealis
)

vita_create_self(localsend-handheld-psv.self localsend-handheld-psv UNSAFE NOASLR)
vita_create_vpk(localsend-handheld.vpk ${VITA_TITLEID} localsend-handheld-psv.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${PSV_ASSETS_FILES}
)
