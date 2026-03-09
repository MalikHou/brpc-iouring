cmake_minimum_required(VERSION 3.16)
project(brpc_iouring_vs_pread CXX)

option(BUILD_SERVER "Build read_bench_server" ON)
option(BUILD_CLIENT "Build read_bench_client" ON)

if(NOT BUILD_SERVER AND NOT BUILD_CLIENT)
  message(FATAL_ERROR "At least one of BUILD_SERVER/BUILD_CLIENT must be ON")
endif()

if(BUILD_SERVER)
  add_subdirectory(server)
endif()

if(BUILD_CLIENT)
  add_subdirectory(client)
endif()
