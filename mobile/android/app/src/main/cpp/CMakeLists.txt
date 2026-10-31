cmake_minimum_required(VERSION 3.24)
project("idr_core_jni" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IDR_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../core")

include(FetchContent)

# Build GeographicLib for the Android target ABI; host libraries cannot be linked into an APK.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build GeographicLib statically" FORCE)
set(BUILD_BOTH_LIBS OFF CACHE BOOL "Build only the static GeographicLib library" FORCE)
set(BUILD_DOCUMENTATION OFF CACHE BOOL "Do not build GeographicLib documentation" FORCE)
set(BUILD_MANPAGES OFF CACHE BOOL "Do not build GeographicLib man pages" FORCE)
set(BINDIR OFF CACHE STRING "Do not build GeographicLib command-line tools" FORCE)
set(EXAMPLEDIR OFF CACHE STRING "Do not build GeographicLib examples" FORCE)
set(USE_BOOST OFF CACHE BOOL "Do not use optional Boost dependencies" FORCE)
set(GEOGRAPHICLIB_PRECISION 2 CACHE STRING "Use double precision" FORCE)

FetchContent_Populate(geographiclib
  URL https://github.com/geographiclib/geographiclib/archive/refs/tags/r2.7.tar.gz
  URL_HASH SHA256=5AACA14CD75F13AA2690273D76E88E8DDF4AEB267D6BE2C3E0BA948DEB9CEEA5
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

set(IDR_GEOGRAPHICLIB_SOURCE_DIR "${geographiclib_SOURCE_DIR}")
if(NOT EXISTS "${IDR_GEOGRAPHICLIB_SOURCE_DIR}/CMakeLists.txt")
  set(IDR_GEOGRAPHICLIB_SOURCE_DIR "${geographiclib_SOURCE_DIR}/geographiclib-r2.7")
endif()
if(NOT EXISTS "${IDR_GEOGRAPHICLIB_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "The pinned GeographicLib source archive could not be located")
endif()

add_subdirectory(
  "${IDR_GEOGRAPHICLIB_SOURCE_DIR}"
  "${geographiclib_BINARY_DIR}"
  EXCLUDE_FROM_ALL
)

add_subdirectory("${IDR_CORE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/idr_core")

find_library(ANDROID_LOG_LIB log)
if(NOT ANDROID_LOG_LIB)
  message(FATAL_ERROR "Android log library was not found in the NDK sysroot")
endif()

add_library(idr_core_jni SHARED idr_jni.cpp)
target_include_directories(idr_core_jni PRIVATE
  "${IDR_CORE_DIR}/include"
  "${IDR_CORE_DIR}/src"
)
target_link_libraries(idr_core_jni PRIVATE idr_core ${ANDROID_LOG_LIB})
