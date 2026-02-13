cmake_minimum_required(VERSION 3.20)

set(FTR_TRACE_IS_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(FTR_TRACE_IS_TOP_LEVEL ON)
  project(ftr_trace LANGUAGES C CXX)
endif()

include(GNUInstallDirs)
set(FTR_TRACE_LZ4_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lz4")
set(FTR_TRACE_LZ4_SRC "${FTR_TRACE_LZ4_DIR}/lz4.c")
set(FTR_TRACE_LZ4_HDR "${FTR_TRACE_LZ4_DIR}/lz4.h")

if(NOT EXISTS "${FTR_TRACE_LZ4_SRC}" OR NOT EXISTS "${FTR_TRACE_LZ4_HDR}")
  message(FATAL_ERROR "Bundled LZ4 sources not found in "
          "${FTR_TRACE_LZ4_DIR}")
endif()

add_library(ftr_trace_lz4 STATIC "${FTR_TRACE_LZ4_SRC}")
target_include_directories(ftr_trace_lz4 PUBLIC "${FTR_TRACE_LZ4_DIR}")
add_library(lz4::lz4 ALIAS ftr_trace_lz4)

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
