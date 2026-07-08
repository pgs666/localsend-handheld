cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "VitaSDK toolchain")
  else()
    message(FATAL_ERROR "Set VITASDK or pass CMAKE_TOOLCHAIN_FILE")
  endif()
endif()

project(localsend_handheld_psv C)

include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "LocalSend Handheld")
set(VITA_TITLEID "LSHD00001")
set(VITA_VERSION "01.00")

add_executable(localsend-handheld-psv
  src/main.c
)

target_link_libraries(localsend-handheld-psv
  SceAppUtil_stub
  SceCommonDialog_stub
  SceGxm_stub
  SceDisplay_stub
  SceCtrl_stub
  SceTouch_stub
  ScePower_stub
  SceAVConfig_stub
  SceRegistryMgr_stub
  -Wl,--whole-archive -lpthread -Wl,--no-whole-archive
)

vita_create_self(localsend-handheld-psv.self localsend-handheld-psv UNSAFE NOASLR)
vita_create_vpk(localsend-handheld.vpk ${VITA_TITLEID} localsend-handheld-psv.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE assets/icon0.png sce_sys/icon0.png
)
