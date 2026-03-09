cmake_minimum_required(VERSION 3.16)
project(read_bench_client CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FORCE_STATIC_THIRD_PARTY "Prefer static third-party libraries from third_party/" ON)

get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(BRPC_ROOT "${PROJECT_ROOT}/third_party/src/brpc/output" CACHE PATH "brpc install root")
set(DEPS_ROOT "${PROJECT_ROOT}/third_party/install" CACHE PATH "dependency install root")

if(FORCE_STATIC_THIRD_PARTY)
  set(_CMAKE_FIND_LIBRARY_SUFFIXES_SAVED "${CMAKE_FIND_LIBRARY_SUFFIXES}")
  set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
endif()

find_path(BRPC_INCLUDE_DIR brpc/server.h
  HINTS "${BRPC_ROOT}" PATH_SUFFIXES include REQUIRED)
find_library(BRPC_LIBRARY brpc
  HINTS "${BRPC_ROOT}" PATH_SUFFIXES lib lib64 REQUIRED)

find_path(PROTOBUF_INCLUDE_DIR google/protobuf/service.h
  HINTS "${DEPS_ROOT}" PATH_SUFFIXES include REQUIRED)
find_library(PROTOBUF_LIBRARY protobuf
  HINTS "${DEPS_ROOT}" PATH_SUFFIXES lib lib64 REQUIRED)
if(EXISTS "${DEPS_ROOT}/bin/protoc")
  set(PROTOC_EXECUTABLE "${DEPS_ROOT}/bin/protoc" CACHE FILEPATH "Path to protoc executable" FORCE)
else()
  find_program(PROTOC_EXECUTABLE protoc
    HINTS "${DEPS_ROOT}" PATH_SUFFIXES bin REQUIRED)
endif()

find_path(GFLAGS_INCLUDE_DIR gflags/gflags.h
  HINTS "${DEPS_ROOT}" PATH_SUFFIXES include REQUIRED)
find_library(GFLAGS_LIBRARY gflags
  HINTS "${DEPS_ROOT}" PATH_SUFFIXES lib lib64 REQUIRED)

find_path(LIBURING_INCLUDE_DIR liburing.h
  HINTS "${DEPS_ROOT}" PATH_SUFFIXES include REQUIRED)
find_library(LIBURING_LIBRARY uring
  HINTS "${DEPS_ROOT}" PATH_SUFFIXES lib lib64 REQUIRED)

find_library(LEVELDB_LIBRARY leveldb
  HINTS "${DEPS_ROOT}" PATH_SUFFIXES lib lib64 REQUIRED)
find_library(SNAPPY_LIBRARY snappy
  HINTS "${DEPS_ROOT}" PATH_SUFFIXES lib lib64 REQUIRED)
find_library(ZLIB_LIBRARY z
  HINTS "${DEPS_ROOT}" PATH_SUFFIXES lib lib64 REQUIRED)
find_library(OPENSSL_SSL_LIBRARY ssl
  HINTS "${DEPS_ROOT}" PATH_SUFFIXES lib lib64 REQUIRED)
find_library(OPENSSL_CRYPTO_LIBRARY crypto
  HINTS "${DEPS_ROOT}" PATH_SUFFIXES lib lib64 REQUIRED)

if(FORCE_STATIC_THIRD_PARTY)
  set(CMAKE_FIND_LIBRARY_SUFFIXES "${_CMAKE_FIND_LIBRARY_SUFFIXES_SAVED}")
endif()

find_package(Threads REQUIRED)
find_library(DL_LIBRARY dl)
find_library(RT_LIBRARY rt)

set(PROTO_FILE "${PROJECT_ROOT}/file_read.proto")
set(PROTO_SRCS "${CMAKE_BINARY_DIR}/file_read.pb.cc")
set(PROTO_HDRS "${CMAKE_BINARY_DIR}/file_read.pb.h")

add_custom_command(
  OUTPUT "${PROTO_SRCS}" "${PROTO_HDRS}"
  COMMAND "${PROTOC_EXECUTABLE}"
  ARGS
    --proto_path=${PROJECT_ROOT}
    --cpp_out=${CMAKE_BINARY_DIR}
    ${PROTO_FILE}
  DEPENDS "${PROTO_FILE}"
  VERBATIM)

add_library(file_read_proto_client STATIC "${PROTO_SRCS}" "${PROTO_HDRS}")
target_include_directories(file_read_proto_client PUBLIC
  "${CMAKE_BINARY_DIR}"
  "${PROTOBUF_INCLUDE_DIR}")
target_link_libraries(file_read_proto_client PUBLIC "${PROTOBUF_LIBRARY}")

set(COMMON_INCLUDE_DIRS
  "${CMAKE_BINARY_DIR}"
  "${BRPC_INCLUDE_DIR}"
  "${PROTOBUF_INCLUDE_DIR}"
  "${GFLAGS_INCLUDE_DIR}"
  "${LIBURING_INCLUDE_DIR}")

set(COMMON_LIBS
  file_read_proto_client
  "${BRPC_LIBRARY}"
  "${GFLAGS_LIBRARY}"
  "${PROTOBUF_LIBRARY}"
  "${LEVELDB_LIBRARY}"
  "${SNAPPY_LIBRARY}"
  "${OPENSSL_SSL_LIBRARY}"
  "${OPENSSL_CRYPTO_LIBRARY}"
  "${ZLIB_LIBRARY}"
  "${LIBURING_LIBRARY}"
  Threads::Threads)

if(DL_LIBRARY)
  list(APPEND COMMON_LIBS "${DL_LIBRARY}")
endif()
if(RT_LIBRARY)
  list(APPEND COMMON_LIBS "${RT_LIBRARY}")
endif()

add_executable(read_bench_client main.cpp)
target_include_directories(read_bench_client PRIVATE ${COMMON_INCLUDE_DIRS})
target_link_libraries(read_bench_client PRIVATE ${COMMON_LIBS})
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_link_options(read_bench_client PRIVATE -no-pie)
endif()

if(NOT FORCE_STATIC_THIRD_PARTY)
  set(RUNTIME_LIB_DIRS "${BRPC_ROOT}/lib" "${DEPS_ROOT}/lib")
  set_target_properties(read_bench_client PROPERTIES
    BUILD_RPATH "${RUNTIME_LIB_DIRS}"
    INSTALL_RPATH "${RUNTIME_LIB_DIRS}")
endif()
