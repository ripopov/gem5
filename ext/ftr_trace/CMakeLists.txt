cmake_minimum_required(VERSION 3.20)

set(FTR_TRACE_IS_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(FTR_TRACE_IS_TOP_LEVEL ON)
  project(ftr_trace LANGUAGES CXX)
endif()

include(GNUInstallDirs)
find_package(lz4 QUIET)
if(NOT TARGET lz4::lz4)
  find_package(LZ4 QUIET)
  if(TARGET LZ4::lz4)
    add_library(lz4::lz4 ALIAS LZ4::lz4)
  elseif(TARGET LZ4::LZ4)
    add_library(lz4::lz4 ALIAS LZ4::LZ4)
  endif()
endif()

if(NOT TARGET lz4::lz4)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(LIBLZ4 QUIET IMPORTED_TARGET liblz4)
    if(TARGET PkgConfig::LIBLZ4)
      add_library(lz4::lz4 ALIAS PkgConfig::LIBLZ4)
    endif()
  endif()
endif()

if(NOT TARGET lz4::lz4)
  if(NOT TARGET lz4_static AND NOT TARGET lz4)
    include(FetchContent)
    set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
    set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
      lz4
      GIT_REPOSITORY https://github.com/lz4/lz4.git
      GIT_TAG v1.10.0
      SOURCE_SUBDIR build/cmake
    )
    FetchContent_MakeAvailable(lz4)
  endif()

  if(NOT TARGET lz4::lz4)
    if(TARGET lz4_static)
      add_library(lz4::lz4 ALIAS lz4_static)
    elseif(TARGET lz4)
      add_library(lz4::lz4 ALIAS lz4)
    endif()
  endif()
endif()

if(NOT TARGET lz4::lz4)
  message(FATAL_ERROR "ftr_trace requires an lz4::lz4 target")
endif()

add_library(ftr_trace INTERFACE)
add_library(ftr_trace::ftr_trace ALIAS ftr_trace)

target_include_directories(ftr_trace INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(ftr_trace INTERFACE lz4::lz4)

if(NOT TARGET ftr)
  add_library(ftr INTERFACE)
  target_link_libraries(ftr INTERFACE ftr_trace)
endif()

option(FTR_TRACE_BUILD_EXAMPLES "Build ftr_trace examples" ${FTR_TRACE_IS_TOP_LEVEL})
if(FTR_TRACE_BUILD_EXAMPLES)
  add_subdirectory(example)
endif()
