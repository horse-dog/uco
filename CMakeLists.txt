cmake_minimum_required(VERSION 3.20)
project(uco)

set(CMAKE_CXX_STANDARD 20)
add_compile_definitions(PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

find_package(Protobuf REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
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
  core/uredis.cpp
  core/string_utils.cpp
)
target_link_libraries(uco PRIVATE uhook PUBLIC OpenSSL::Crypto)

# ---- usql: 基于 io_uring + 协程的异步 MySQL 支持 (可选, 自动检测) ----
option(USE_MYSQL "Enable usql (async MySQL C API support)" ON)
if(USE_MYSQL)
  find_path(USQL_MYSQL_INCLUDE_DIR mysql/mysql.h)
  find_library(USQL_MYSQL_LIBRARY NAMES mysqlclient mariadbclient mariadb)
  if(USQL_MYSQL_INCLUDE_DIR AND USQL_MYSQL_LIBRARY)
    message(STATUS "MySQL client found: ${USQL_MYSQL_LIBRARY}, usql enabled")
    target_sources(uco PRIVATE core/usql.cpp)
    target_include_directories(uco PUBLIC ${USQL_MYSQL_INCLUDE_DIR})
    target_link_libraries(uco PUBLIC ${USQL_MYSQL_LIBRARY})
  else()
    message(STATUS "MySQL client not found, usql disabled")
  endif()
endif()

file(GLOB_RECURSE SRC_HTTP "src/*.cpp")
# 业务层 (controller/service/dao + main); 新增文件需重跑 cmake configure.
file(GLOB_RECURSE SRC_WEBSERVER "webserver/*.cpp")

add_executable(
    server 
    ${PROTO_SRCS}
    ${PROTO_HDRS}
    ${SRC_HTTP}
    ${SRC_WEBSERVER}
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

add_executable(test_usleeper tests/test_usleeper.cpp)
target_link_libraries(test_usleeper PRIVATE ulog uco uring)

add_executable(test_uredis tests/test_uredis.cpp)
target_link_libraries(test_uredis PRIVATE ulog uco uring)

if(USQL_MYSQL_INCLUDE_DIR AND USQL_MYSQL_LIBRARY)
  add_executable(test_usql ${PROTO_SRCS} ${PROTO_HDRS} tests/test_usql.cpp)
  target_link_libraries(test_usql PRIVATE ulog uco uring)
endif()
