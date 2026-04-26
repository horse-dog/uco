cmake_minimum_required(VERSION 3.20)
project(uco)

set(CMAKE_CXX_STANDARD 20)
add_compile_definitions(PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

find_package(Protobuf REQUIRED)
file(GLOB_RECURSE PROTO_FILES "proto/*.proto")
set(GENERATED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/gen)
file(MAKE_DIRECTORY ${GENERATED_DIR})

option(USE_JEMALLOC "Use jemalloc" OFF)

protobuf_generate_cpp(
    PROTO_SRCS PROTO_HDRS ${PROTO_FILES}
    PROTOC_OUT_DIR ${GENERATED_DIR}
)

include_directories(
    ${Protobuf_INCLUDE_DIRS}
    ${GENERATED_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/core
)

add_library(ulog core/ulogshm.cpp core/ulog.cpp)
target_link_libraries(ulog PRIVATE ${Protobuf_LIBRARIES})

add_library(uhook SHARED core/uhook.cpp)

add_library(
  uco
  core/uco.cpp
  core/uio.cpp
  core/usync.cpp
  core/udaemon.cpp
)
target_link_libraries(uco PRIVATE uhook)

file(GLOB_RECURSE SRC_HTTP "src/*.cpp")

add_executable(
    server 
    ${PROTO_SRCS}
    ${PROTO_HDRS}
    ${SRC_HTTP}
    main.cpp
)
target_link_libraries(server PRIVATE ulog uco uring)
if(USE_JEMALLOC)
    target_link_libraries(server PRIVATE jemalloc)
endif()

add_executable(ulogcsm core/udaemon.cpp core/ulogcsm.cpp)
target_link_libraries(ulogcsm PRIVATE ulog)

add_executable(test_ulog tests/test_ulog.cpp)
target_link_libraries(test_ulog PRIVATE ulog)

add_executable(test_log_concurrent tests/test_ulog_concurrent.cpp)
target_link_libraries(test_log_concurrent PRIVATE ulog)

add_executable(test_ubatch tests/test_ubatch.cpp)
target_link_libraries(test_ubatch PRIVATE ulog uco uring)

add_executable(test_ucond tests/test_ucond.cpp)
target_link_libraries(test_ucond PRIVATE ulog uco uring)

add_executable(test_umutex tests/test_umutex.cpp)
target_link_libraries(test_umutex PRIVATE ulog uco uring)

add_executable(test_ushared_mutex tests/test_ushared_mutex.cpp)
target_link_libraries(test_ushared_mutex PRIVATE ulog uco uring)

add_executable(test_uchan tests/test_uchan.cpp)
target_link_libraries(test_uchan PRIVATE ulog uco uring)
