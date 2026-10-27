cmake_minimum_required(VERSION 3.24)
project(idr_core VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Optimization and warning flags
if(MSVC)
  add_compile_options(/W4 /O2 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -O3)
endif()

# Include directories
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Find Eigen3
find_package(Eigen3 3.3 QUIET)
if(NOT Eigen3_FOUND)
  find_path(EIGEN3_INCLUDE_DIR NAMES signature_of_eigen3_matrix_library PATH_SUFFIXES eigen3 Eigen
            PATHS /usr/include/eigen3 /usr/local/include/eigen3 /opt/homebrew/include/eigen3)
  if(EIGEN3_INCLUDE_DIR)
    include_directories(${EIGEN3_INCLUDE_DIR})
    message(STATUS "Found Eigen3 headers: ${EIGEN3_INCLUDE_DIR}")
  elseif(WIN32 AND EXISTS "C:/msys64/ucrt64/include/eigen3")
    include_directories("C:/msys64/ucrt64/include/eigen3")
    message(STATUS "Using Eigen3 from C:/msys64/ucrt64/include/eigen3")
  else()
    message(FATAL_ERROR "Eigen3 not found")
  endif()
else()
  include_directories(${EIGEN3_INCLUDE_DIR})
endif()

# Find SQLite3
find_package(SQLite3 REQUIRED)

# Find GeographicLib
find_package(GeographicLib QUIET)
if(TARGET GeographicLib::GeographicLib)
  set(GEOGRAPHICLIB_LIBRARIES GeographicLib::GeographicLib)
elseif(GeographicLib_LIBRARIES)
  set(GEOGRAPHICLIB_LIBRARIES ${GeographicLib_LIBRARIES})
else()
  find_path(GEOGRAPHICLIB_INCLUDE_DIR GeographicLib/Config.h PATH_SUFFIXES GeographicLib
            PATHS /usr/include /usr/local/include /opt/homebrew/include)
  find_library(GEOGRAPHICLIB_LIB NAMES GeographicLib Geographic
               PATHS /usr/lib /usr/local/lib /opt/homebrew/lib)
  if(GEOGRAPHICLIB_INCLUDE_DIR AND GEOGRAPHICLIB_LIB)
    include_directories(${GEOGRAPHICLIB_INCLUDE_DIR})
    set(GEOGRAPHICLIB_LIBRARIES ${GEOGRAPHICLIB_LIB})
    message(STATUS "Found GeographicLib: ${GEOGRAPHICLIB_LIB}")
  elseif(WIN32 AND EXISTS "C:/msys64/ucrt64/include/GeographicLib")
    include_directories("C:/msys64/ucrt64/include")
    link_directories("C:/msys64/ucrt64/lib")
    set(GEOGRAPHICLIB_LIBRARIES GeographicLib)
    message(STATUS "Using GeographicLib from C:/msys64/ucrt64")
  endif()
endif()

# ONNX Runtime (C++ API)
if(DEFINED ENV{ORT_ROOT} AND EXISTS "$ENV{ORT_ROOT}")
  set(ORT_ROOT "$ENV{ORT_ROOT}")
else()
  set(ORT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime/onnxruntime-win-x64-1.19.2")
endif()
if(EXISTS "${ORT_ROOT}")
  include_directories("${ORT_ROOT}/include")
  link_directories("${ORT_ROOT}/lib")
  set(ORT_LIBRARIES onnxruntime)
  add_compile_definitions(ORT_API_VERSION=17)
  message(STATUS "Using ONNX Runtime from ${ORT_ROOT}")
endif()

# Core sources
file(GLOB_RECURSE IDR_SOURCES
  "src/*.cpp"
)

add_library(idr_core STATIC ${IDR_SOURCES})
target_compile_definitions(idr_core PUBLIC IDR_STATIC)
target_link_libraries(idr_core PUBLIC SQLite3::SQLite3)
if(GEOGRAPHICLIB_LIBRARIES)
  target_link_libraries(idr_core PUBLIC ${GEOGRAPHICLIB_LIBRARIES})
endif()
if(ORT_LIBRARIES)
  target_link_libraries(idr_core PUBLIC ${ORT_LIBRARIES})
endif()

# Tools: replay_cli
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/replay_cli/main.cpp")
  add_executable(replay_cli tools/replay_cli/main.cpp)
  target_link_libraries(replay_cli PRIVATE idr_core)
  if(MINGW)
    target_link_options(replay_cli PRIVATE -mconsole)
  endif()
endif()

# Tools: ubodt_gen
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/ubodt_gen/main.cpp")
  add_executable(ubodt_gen tools/ubodt_gen/main.cpp)
  target_link_libraries(ubodt_gen PRIVATE idr_core SQLite3::SQLite3)
  if(MINGW)
    target_link_options(ubodt_gen PRIVATE -mconsole)
  endif()
endif()

# Tests
enable_testing()
find_package(GTest QUIET)
if(NOT GTest_FOUND)
  if(WIN32 AND EXISTS "C:/msys64/ucrt64/include/gtest")
    include_directories("C:/msys64/ucrt64/include")
    link_directories("C:/msys64/ucrt64/lib")
    set(GTEST_LIBRARIES gtest gtest_main)
    set(GTest_FOUND TRUE)
  endif()
endif()

if(GTest_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit")
  file(GLOB_RECURSE TEST_SOURCES "tests/unit/*.cpp")
  if(TEST_SOURCES)
    add_executable(idr_unit_tests ${TEST_SOURCES})
    if(TARGET GTest::gtest)
      target_link_libraries(idr_unit_tests PRIVATE idr_core GTest::gtest GTest::gtest_main)
    else()
      target_link_libraries(idr_unit_tests PRIVATE idr_core ${GTEST_LIBRARIES})
    endif()
    if(MINGW)
      target_link_options(idr_unit_tests PRIVATE -mconsole)
    endif()
    if(EXISTS "${ORT_ROOT}/lib/onnxruntime.dll")
      add_custom_command(TARGET idr_unit_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${ORT_ROOT}/lib/onnxruntime.dll"
          "$<TARGET_FILE_DIR:idr_unit_tests>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${ORT_ROOT}/lib/onnxruntime_providers_shared.dll"
          "$<TARGET_FILE_DIR:idr_unit_tests>"
      )
    endif()
    add_test(NAME UnitTests COMMAND idr_unit_tests)
  endif()
endif()
