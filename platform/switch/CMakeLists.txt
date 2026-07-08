cmake_minimum_required(VERSION 3.10)

set(PLATFORM_SWITCH ON CACHE BOOL "" FORCE)
set(USE_DEKO3D ON CACHE BOOL "" FORCE)
set(BOREALIS_USE_DEKO3D ON CACHE BOOL "" FORCE)
set(USE_GLFW OFF CACHE BOOL "" FORCE)
set(USE_SDL2 OFF CACHE BOOL "" FORCE)
set(USE_LIBROMFS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(USE_SYSTEM_FMT OFF CACHE BOOL "" FORCE)
set(USE_SYSTEM_TINYXML2 OFF CACHE BOOL "" FORCE)
set(USE_SYSTEM_TWEENY OFF CACHE BOOL "" FORCE)
set(BRLS_UNITY_BUILD OFF CACHE BOOL "" FORCE)

if(NOT DEFINED DEVKITPRO)
  if(DEFINED ENV{DEVKITPRO})
    set(DEVKITPRO "$ENV{DEVKITPRO}" CACHE PATH "devkitPro path")
  else()
    message(FATAL_ERROR "Set DEVKITPRO")
  endif()
endif()

include("${DEVKITPRO}/cmake/Switch.cmake" REQUIRED)

project(localsend_handheld_switch_ui C CXX)

set(PROJECT_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(BOREALIS_LIBRARY ${PROJECT_ROOT}/third_party/borealis/library)
include(${PROJECT_ROOT}/cmake/LocalSendCoreSources.cmake)
include(${PROJECT_ROOT}/cmake/LocalSendHandheldUi.cmake)
set(APP_TITLE "LocalSend Handheld")
set(APP_AUTHOR "pgs666")
set(APP_VERSION "0.1.0")

if(USE_DEKO3D)
  find_program(NX_UAM_EXE NAMES uam HINTS "${DEVKITPRO}/tools/bin")
  if(NOT NX_UAM_EXE)
    message(FATAL_ERROR "Could not find uam: install libuam before building the Switch deko3d target")
  endif()
endif()

add_subdirectory(${BOREALIS_LIBRARY}
  ${CMAKE_CURRENT_BINARY_DIR}/borealis)

target_include_directories(borealis PUBLIC
  ${DEVKITPRO}/portlibs/switch/include
)

target_link_directories(borealis PUBLIC
  ${DEVKITPRO}/portlibs/switch/lib
)

add_library(localsend-core-switch-ui STATIC ${LOCALSEND_CORE_SOURCES})

set_target_properties(localsend-core-switch-ui PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
)

target_include_directories(localsend-core-switch-ui PUBLIC
  ${PROJECT_ROOT}/include
)

target_compile_definitions(localsend-core-switch-ui PUBLIC
  LOCALSEND_HAS_MBEDTLS=0
  LOCALSEND_PLATFORM_SWITCH=1
)

target_compile_options(localsend-core-switch-ui PRIVATE
  -ffunction-sections
  -fdata-sections
)

add_executable(localsend-handheld-switch-ui
  ${LOCALSEND_HANDHELD_UI_SOURCES}
  ${PROJECT_ROOT}/third_party/borealis/library/lib/platforms/switch/switch_wrapper.c
  src/ui_main.cpp
)

set_target_properties(localsend-handheld-switch-ui PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
)

target_include_directories(localsend-handheld-switch-ui PRIVATE
  ${LOCALSEND_HANDHELD_UI_INCLUDE_DIRS}
)

target_compile_options(localsend-handheld-switch-ui PRIVATE
  -ffunction-sections
  -fdata-sections
)

target_link_libraries(localsend-handheld-switch-ui PRIVATE
  borealis
  localsend-core-switch-ui
)

add_custom_target(localsend-handheld-switch-ui.nro ALL DEPENDS localsend-handheld-switch-ui
  COMMAND ${NX_NACPTOOL_EXE} --create "${APP_TITLE}" "${APP_AUTHOR}" "${APP_VERSION}" localsend-handheld-switch-ui.nacp
  COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_CURRENT_BINARY_DIR}/resources
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_ROOT}/third_party/borealis/resources ${CMAKE_CURRENT_BINARY_DIR}/resources
  COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_CURRENT_BINARY_DIR}/resources/font
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/resources/shaders
  COMMAND ${NX_UAM_EXE} -s vert -o ${CMAKE_CURRENT_BINARY_DIR}/resources/shaders/fill_vsh.dksh ${BOREALIS_LIBRARY}/lib/extern/nanovg/deko3d/shaders/fill_vsh.glsl
  COMMAND ${NX_UAM_EXE} -s frag -o ${CMAKE_CURRENT_BINARY_DIR}/resources/shaders/fill_aa_fsh.dksh ${BOREALIS_LIBRARY}/lib/extern/nanovg/deko3d/shaders/fill_aa_fsh.glsl
  COMMAND ${NX_UAM_EXE} -s frag -o ${CMAKE_CURRENT_BINARY_DIR}/resources/shaders/fill_fsh.dksh ${BOREALIS_LIBRARY}/lib/extern/nanovg/deko3d/shaders/fill_fsh.glsl
  COMMAND ${NX_ELF2NRO_EXE} localsend-handheld-switch-ui.elf localsend-handheld.nro
          --icon=${CMAKE_CURRENT_LIST_DIR}/assets/icon.png
          --nacp=localsend-handheld-switch-ui.nacp
          --romfsdir=${CMAKE_CURRENT_BINARY_DIR}/resources
)
