cmake_minimum_required(VERSION 3.20)
project(av1mkv CXX C)

# CineForm to AV1/Matroska, Windows only (NVENC through the NVIDIA driver, a D3D11 device
# on the RTX 4090). Built with llvm-mingw clang by build.sh.
#
# Dependencies, all from github.com/V-Sekai-fire and vendored as squashed subtrees under
# thirdparty/ (rule 1 of AGENTS.md):
#   cineform-sdk  the CFHD decoder (libCFHDCodecStatic, built here from source)
#   libwebm       mkvmuxer (writing) and mkvparser (the `info` verification), compiled in
# nv-codec-headers come from the copy tools/oxrsys already carries.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# The SDK looks for OpenMP and adds -fopenmp to its flags when found. llvm-mingw ships
# only the import library for libomp, which a -static link cannot take, and this tool's
# decode is not the bottleneck (see README): OpenMP is left out of the lookup.
set(CMAKE_DISABLE_FIND_PACKAGE_OpenMP ON)
set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_STATIC ON CACHE BOOL "" FORCE)
set(BUILD_SEPARATED OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/cineform-sdk EXCLUDE_FROM_ALL)
# clang 21 makes these errors by default; the SDK passes int32_t* where Win32 wants LONG*
# (ReleaseSemaphore in Codec/decoder.c). The same two flags entities-godot-cineform builds
# the same sources with.
target_compile_options(CFHDCodecStatic PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-pointer-types -Wno-incompatible-function-pointer-types>
  -w)

# libFLAC from the org's fork of xiph/flac (1.5.0, BSD-3 for the library): the library only, no
# programs (those are GPL), no Ogg, no C++ bindings.
foreach(opt BUILD_PROGRAMS BUILD_EXAMPLES BUILD_TESTING BUILD_DOCS BUILD_CXXLIBS WITH_OGG INSTALL_MANPAGES INSTALL_PKGCONFIG_MODULES INSTALL_CMAKE_CONFIG_MODULE ENABLE_MULTITHREADING)
  set(${opt} OFF CACHE BOOL "" FORCE)
endforeach()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/flac EXCLUDE_FROM_ALL)

set(LIBWEBM thirdparty/libwebm)
add_library(webm_muxparse STATIC
  ${LIBWEBM}/mkvmuxer/mkvmuxer.cc
  ${LIBWEBM}/mkvmuxer/mkvmuxerutil.cc
  ${LIBWEBM}/mkvmuxer/mkvwriter.cc
  ${LIBWEBM}/mkvparser/mkvparser.cc
  ${LIBWEBM}/mkvparser/mkvreader.cc)
target_include_directories(webm_muxparse PUBLIC ${LIBWEBM})

add_executable(av1mkv
  src/main.cpp
  src/flac_track.cpp
  src/mkv_info.cpp
  src/mkv_tracks.cpp
  src/nvenc_av1.cpp)
# V-Sekai-fire/nv-codec-headers: the copy tools/oxrsys carries (untracked there, fetched by
# its build), else the one in the main checkout; NV_CODEC_HEADERS overrides.
set(NV_CODEC_HEADERS "" CACHE PATH "nv-codec-headers include directory")
if(NV_CODEC_HEADERS STREQUAL "")
  set(NV_CODEC_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/../oxrsys/third_party/nv-codec-headers/include)
  if(NOT EXISTS ${NV_CODEC_HEADERS}/ffnvcodec/nvEncodeAPI.h)
    set(NV_CODEC_HEADERS C:/interactor-dress-on/tools/oxrsys/third_party/nv-codec-headers/include)
  endif()
endif()
target_include_directories(av1mkv PRIVATE
  thirdparty/cineform-sdk/Common
  ${NV_CODEC_HEADERS})
target_compile_definitions(av1mkv PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_link_libraries(av1mkv PRIVATE webm_muxparse CFHDCodecStatic FLAC d3d11 dxgi)
# One self-contained exe: no libc++ or libunwind DLLs next to it.
# The SDK archive defines GetProcessorCount in both its encoder and decoder objects; both
# objects are pulled in, so the linker is told to take the first.
target_link_options(av1mkv PRIVATE -static -Wl,--allow-multiple-definition)
