cmake_minimum_required(VERSION 3.18)

project(localsend_handheld VERSION 0.1.0 LANGUAGES C CXX)

option(PLATFORM_DESKTOP "Build the desktop protocol prototype" ON)
option(PLATFORM_SWITCH "Build the Nintendo Switch homebrew target" OFF)
option(PLATFORM_PSV "Build the PlayStation Vita homebrew target" OFF)
option(LOCALSEND_BUILD_TESTS "Build desktop protocol tests" ON)

if(NOT PLATFORM_DESKTOP AND NOT PLATFORM_SWITCH AND NOT PLATFORM_PSV)
  message(FATAL_ERROR "Enable at least one platform: PLATFORM_DESKTOP, PLATFORM_SWITCH, or PLATFORM_PSV")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ENABLE_PROGRAMS OFF CACHE BOOL "Do not build Mbed TLS programs" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "Do not build Mbed TLS tests" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "Do not treat Mbed TLS warnings as fatal" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "Do not build shared Mbed TLS libraries" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "Build static Mbed TLS libraries" FORCE)
add_subdirectory(third_party/mbedtls EXCLUDE_FROM_ALL)

add_library(localsend_core
  src/core/app_service.cpp
  src/core/config.cpp
  src/core/device_store.cpp
  src/core/discovery.cpp
  src/core/file_browser.cpp
  src/core/http.cpp
  src/core/json.cpp
  src/core/protocol.cpp
  src/core/safe_path.cpp
  src/core/security.cpp
  src/core/tls.cpp
  src/core/transfer.cpp
)

target_include_directories(localsend_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(localsend_core PUBLIC cxx_std_17)
find_package(Threads REQUIRED)
target_link_libraries(localsend_core PUBLIC Threads::Threads mbedtls mbedx509 mbedcrypto)
target_compile_definitions(localsend_core PUBLIC LOCALSEND_HAS_MBEDTLS=1)

if(MSVC)
  target_compile_options(localsend_core PRIVATE /W4)
else()
  target_compile_options(localsend_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PLATFORM_SWITCH)
  add_library(localsend_platform_switch INTERFACE)
  target_compile_definitions(localsend_platform_switch INTERFACE LOCALSEND_PLATFORM_SWITCH=1)
  message(STATUS "Switch packaging is staged; provide devkitPro/libnx toolchain to build .nro")
endif()

if(PLATFORM_PSV)
  add_library(localsend_platform_psv INTERFACE)
  target_compile_definitions(localsend_platform_psv INTERFACE LOCALSEND_PLATFORM_PSV=1)
  message(STATUS "PSV packaging is staged; provide vitasdk toolchain to build .vpk")
endif()

if(PLATFORM_DESKTOP)
  add_executable(localsend-desktop app/desktop_main.cpp)
  target_link_libraries(localsend-desktop PRIVATE localsend_core)
  target_compile_definitions(localsend-desktop PRIVATE LOCALSEND_PLATFORM_DESKTOP=1)

  if(LOCALSEND_BUILD_TESTS)
    enable_testing()
    add_executable(localsend-core-tests tests/core_tests.cpp)
    target_link_libraries(localsend-core-tests PRIVATE localsend_core)
    add_test(NAME localsend-core-tests COMMAND localsend-core-tests)
  endif()
endif()
