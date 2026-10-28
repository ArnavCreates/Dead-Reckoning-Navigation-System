cmake_minimum_required(VERSION 3.22.1)
project("idr_core_jni")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IDR_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../core")

include_directories(
  "${IDR_CORE_DIR}/include"
  "${IDR_CORE_DIR}/src"
)

# Look for Eigen3 headers
find_package(Eigen3 3.3 QUIET)
if(NOT Eigen3_FOUND)
  find_path(EIGEN3_INCLUDE_DIR NAMES signature_of_eigen3_matrix_library PATH_SUFFIXES eigen3 Eigen
            PATHS /usr/include/eigen3 /usr/local/include/eigen3 /opt/homebrew/include/eigen3
            NO_CMAKE_FIND_ROOT_PATH)
  if(EIGEN3_INCLUDE_DIR)
    include_directories(${EIGEN3_INCLUDE_DIR})
  endif()
else()
  include_directories(${EIGEN3_INCLUDE_DIRS} ${EIGEN3_INCLUDE_DIR} ${Eigen3_INCLUDE_DIRS})
endif()

find_library(log-lib log)
find_library(sqlite3-lib sqlite3)

add_library(idr_core_jni SHARED
  idr_jni.cpp
  "${IDR_CORE_DIR}/src/engine/engine.cpp"
  "${IDR_CORE_DIR}/src/capi/idr_capi.cpp"
)

target_compile_definitions(idr_core_jni PRIVATE IDR_STATIC)

target_link_libraries(idr_core_jni
  ${log-lib}
  ${sqlite3-lib}
)
