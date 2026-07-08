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
set(BRLS_RESOURCES_DIR "app0:" CACHE STRING "" FORCE)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../third_party/borealis/library
  ${CMAKE_CURRENT_BINARY_DIR}/borealis)

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
