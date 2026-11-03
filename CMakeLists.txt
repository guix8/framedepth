# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 FrameDepth contributors
# FrameDepth companion: builds on the headset (SteamOS, aarch64) after
# scripts/fetch-deps.sh and the LiteRT/ncnn builds in scripts/build.sh.
cmake_minimum_required(VERSION 3.20)
project(framedepth C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# One version for both parts: the extension's manifest. The companion reports
# it, so the popup can tell when Chrome still runs an older extension.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/extension/manifest.json FD_MANIFEST)
string(JSON FD_VERSION GET "${FD_MANIFEST}" version)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS extension/manifest.json)

set(FD_DEPS ${CMAKE_CURRENT_SOURCE_DIR}/build-deps CACHE PATH "fetched and built dependencies")
set(FD_MOONLIGHT ${FD_DEPS}/moonlight-xr CACHE PATH "Moonlight XR for Frame checkout (depth engine)")
set(FD_LITERT_DIR ${FD_DEPS}/litert/dist CACHE PATH "LiteRT C API build (Moonlight's depth/tools/build-litert.sh)")
set(FD_NCNN_DIR ${FD_DEPS}/ncnn/dist CACHE PATH "ncnn install with Vulkan")
# OpenVR's client library comes with SteamVR; only its header is fetched
set(FD_OPENVR_LIB /opt/steamvr/bin/linuxarm64/libopenvr_api.so CACHE FILEPATH "SteamVR's OpenVR client library")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(FD_X11 REQUIRED IMPORTED_TARGET x11 xext xdamage xfixes xcomposite x11-xcb xcb xcb-dri3)
pkg_check_modules(FD_GL REQUIRED IMPORTED_TARGET egl opengl)
find_package(ncnn CONFIG REQUIRED PATHS ${FD_NCNN_DIR}/lib/cmake/ncnn NO_DEFAULT_PATH)

add_library(fd_litert STATIC IMPORTED)
set_target_properties(fd_litert PROPERTIES
  IMPORTED_LOCATION ${FD_LITERT_DIR}/lib/libmlxr_litert.a
  INTERFACE_INCLUDE_DIRECTORIES ${FD_LITERT_DIR}/include
  INTERFACE_LINK_LIBRARIES "pthread;dl;m")

# The depth engine from Moonlight XR for Frame (GPLv3, unmodified) and its
# model backend dispatcher (companion/depth_backend.cpp): ncnn on the GPU for
# *.ncnn.param, Moonlight's LiteRT backend on the CPU otherwise. The LiteRT
# backend is compiled unchanged with its model functions renamed so the
# dispatcher can take their names.
add_library(fd_depth STATIC
  ${FD_MOONLIGHT}/depth/litert_backend.c ${FD_MOONLIGHT}/depth/depth_engine.c
  ${FD_MOONLIGHT}/xr/core/xr_depthmap.c ${FD_MOONLIGHT}/xr/core/xr_rate.c
  companion/depth_backend.cpp)
set(FD_LITERT_RENAMES MlxrDepthModel=MlxrLitertModel)
foreach(f load free width height threads flags load_ms input run output)
  list(APPEND FD_LITERT_RENAMES mlxr_depth_model_${f}=fd_litert_${f})
endforeach()
set_source_files_properties(${FD_MOONLIGHT}/depth/litert_backend.c PROPERTIES COMPILE_DEFINITIONS "${FD_LITERT_RENAMES}")
target_include_directories(fd_depth PUBLIC ${FD_MOONLIGHT}/depth ${FD_MOONLIGHT}/xr/core)
target_link_libraries(fd_depth PUBLIC fd_litert ncnn Threads::Threads m)
set_target_properties(fd_depth PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(fd_depth PRIVATE -O2)

add_executable(framedepth-companion companion/framedepth_companion.cpp companion/gpu_warp.cpp)
target_include_directories(framedepth-companion PRIVATE ${FD_DEPS}/openvr)
target_compile_options(framedepth-companion PRIVATE -Wall -Wextra -O2)
target_compile_definitions(framedepth-companion PRIVATE FD_VERSION="${FD_VERSION}")
target_link_libraries(framedepth-companion PRIVATE fd_depth ${FD_OPENVR_LIB} PkgConfig::FD_X11 PkgConfig::FD_GL)
target_link_options(framedepth-companion PRIVATE -static-libstdc++ -static-libgcc)
get_filename_component(FD_OPENVR_DIR ${FD_OPENVR_LIB} DIRECTORY)
set_target_properties(framedepth-companion PROPERTIES LINKER_LANGUAGE CXX
  BUILD_RPATH ${FD_OPENVR_DIR} INSTALL_RPATH ${FD_OPENVR_DIR})
