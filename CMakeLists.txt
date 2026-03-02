if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.16)
  project(file_read_server_standalone CXX)

  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

  set(THIRD_PARTY_ROOT "${CMAKE_SOURCE_DIR}/third_party/install" CACHE PATH
      "Local third-party install root (contains include/ and lib/)")
  set(BRPC_ROOT "" CACHE PATH "Root path of installed brpc (contains include/ and lib/)")
  if (NOT BRPC_ROOT AND EXISTS "${CMAKE_SOURCE_DIR}/third_party/src/brpc/output")
    set(BRPC_ROOT "${CMAKE_SOURCE_DIR}/third_party/src/brpc/output")
  endif()
  set(CMAKE_BUILD_RPATH "${THIRD_PARTY_ROOT}/lib;${BRPC_ROOT}/lib")
  set(CMAKE_INSTALL_RPATH "${THIRD_PARTY_ROOT}/lib;${BRPC_ROOT}/lib")
  set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
  if (NOT EXISTS "${THIRD_PARTY_ROOT}")
    message(FATAL_ERROR
      "THIRD_PARTY_ROOT not found: ${THIRD_PARTY_ROOT}. Run ./bootstrap_server_deps.sh first.")
  endif()

  find_path(BRPC_INCLUDE_DIR brpc/server.h
    HINTS
      ${BRPC_ROOT}
    PATH_SUFFIXES
      include
    NO_DEFAULT_PATH
  )
  find_library(BRPC_LIBRARY brpc
    HINTS
      ${BRPC_ROOT}
    PATH_SUFFIXES
      lib
      lib64
    NO_DEFAULT_PATH
  )
  if (NOT BRPC_INCLUDE_DIR OR NOT BRPC_LIBRARY)
    message(FATAL_ERROR
      "brpc headers/library not found. Set -DBRPC_ROOT=/path/to/brpc/install")
  endif()

  # brpc headers include google/protobuf/service.h directly.
  # Probe with service.h (not just message.h) to catch mismatched protobuf installs.
  find_path(PROTOBUF_INCLUDE_DIR google/protobuf/service.h
    HINTS ${THIRD_PARTY_ROOT}
    PATH_SUFFIXES include
    NO_DEFAULT_PATH
  )
  find_library(PROTOBUF_LIBRARY protobuf
    HINTS ${THIRD_PARTY_ROOT}
    PATH_SUFFIXES lib lib64
    NO_DEFAULT_PATH
  )
  find_program(PROTOC_EXECUTABLE protoc
    HINTS ${THIRD_PARTY_ROOT}
    PATH_SUFFIXES bin
    NO_DEFAULT_PATH
  )
  if (NOT PROTOBUF_INCLUDE_DIR OR NOT PROTOBUF_LIBRARY OR NOT PROTOC_EXECUTABLE)
    message(FATAL_ERROR
      "protobuf not found under THIRD_PARTY_ROOT=${THIRD_PARTY_ROOT}.")
  endif()

  find_path(GFLAGS_INCLUDE_DIR gflags/gflags.h
    HINTS ${THIRD_PARTY_ROOT}
    PATH_SUFFIXES include
    NO_DEFAULT_PATH
  )
  find_library(GFLAGS_LIBRARY gflags
    HINTS ${THIRD_PARTY_ROOT}
    PATH_SUFFIXES lib lib64
    NO_DEFAULT_PATH
  )
  if (NOT GFLAGS_INCLUDE_DIR OR NOT GFLAGS_LIBRARY)
    message(FATAL_ERROR
      "gflags not found under THIRD_PARTY_ROOT=${THIRD_PARTY_ROOT}.")
  endif()
  set(SERVER_GFLAGS_LIB ${GFLAGS_LIBRARY})

  find_path(LIBURING_INCLUDE_DIR liburing.h
    HINTS ${THIRD_PARTY_ROOT}
    PATH_SUFFIXES include
    NO_DEFAULT_PATH
  )
  find_library(LIBURING_LIBRARY uring
    HINTS ${THIRD_PARTY_ROOT}
    PATH_SUFFIXES lib lib64
    NO_DEFAULT_PATH
  )
  if (NOT LIBURING_INCLUDE_DIR OR NOT LIBURING_LIBRARY)
    message(FATAL_ERROR "liburing not found.")
  endif()

  set(PROTO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/file_read.proto")
  set(PROTO_SRCS "${CMAKE_CURRENT_BINARY_DIR}/file_read.pb.cc")
  set(PROTO_HDRS "${CMAKE_CURRENT_BINARY_DIR}/file_read.pb.h")
  add_custom_command(
    OUTPUT "${PROTO_SRCS}" "${PROTO_HDRS}"
    COMMAND "${PROTOC_EXECUTABLE}"
    ARGS
      "--cpp_out=${CMAKE_CURRENT_BINARY_DIR}"
      "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}"
      "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}"
    COMMENT "Generating file_read.pb.cc/.h from file_read.proto"
    VERBATIM
  )
  add_library(file_read_proto STATIC ${PROTO_SRCS} ${PROTO_HDRS})
  target_include_directories(file_read_proto
    PUBLIC
      ${CMAKE_CURRENT_BINARY_DIR}
      ${PROTOBUF_INCLUDE_DIR}
  )
  # Force explicit include path emission even when environment (e.g. CPATH)
  # makes CMake treat it as an implicit system include directory.
  target_compile_options(file_read_proto PRIVATE "-I${PROTOBUF_INCLUDE_DIR}")
  target_link_libraries(file_read_proto PUBLIC ${PROTOBUF_LIBRARY})
endif()

if (NOT SERVER_GFLAGS_LIB)
  set(SERVER_GFLAGS_LIB gflags)
endif()

add_executable(file_read_server server.cpp)
target_include_directories(file_read_server
  PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${PROTOBUF_INCLUDE_DIR}
    ${BRPC_INCLUDE_DIR}
    ${LIBURING_INCLUDE_DIR}
)
# Force explicit include path emission even when environment (e.g. CPATH)
# makes CMake treat paths as implicit and omit -I.
target_compile_options(file_read_server PRIVATE
  "-I${PROTOBUF_INCLUDE_DIR}"
  "-I${BRPC_INCLUDE_DIR}"
)
target_link_libraries(file_read_server
  PRIVATE
    file_read_proto
    ${BRPC_LIBRARY}
    ${LIBURING_LIBRARY}
    ${SERVER_GFLAGS_LIB}
    pthread
)
