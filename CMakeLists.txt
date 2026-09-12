cmake_minimum_required(VERSION 3.20)
project(uco)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_definitions(PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

# 可选开发工具：构建 UCO clang-tidy 动态插件，以及静态注册该检查的
# 定制 clangd。它们不属于项目运行时依赖，缺少工具链时不得影响正常构建。
option(UCO_BUILD_CLANG_TIDY_PLUGIN
       "Build the UCO project-specific clang-tidy plugin and clangd" ON)
if(UCO_BUILD_CLANG_TIDY_PLUGIN)
  set(UCO_CLANG_TIDY_DEPS_FOUND TRUE)

  # Ubuntu 的 LLVM/Clang CMake 包通常安装在此路径；QUIET 保证未安装
  # 开发包时仅跳过自定义工具，不产生配置错误。
  find_package(LLVM 18.1 QUIET CONFIG
               HINTS /usr/lib/llvm-18/lib/cmake/llvm)
  if(LLVM_FOUND)
    find_package(Clang 18.1 QUIET CONFIG
                 HINTS /usr/lib/llvm-18/lib/cmake/clang)
  endif()

  # 动态检查插件需要 clangTidy/ASTMatchers；定制 clangd 还需要
  # clangdMain。通过目标存在性判断 Clang 开发包是否完整。
  foreach(DEP_TARGET clangTidy clangASTMatchers clangdMain)
    if(NOT TARGET ${DEP_TARGET})
      set(UCO_CLANG_TIDY_DEPS_FOUND FALSE)
    endif()
  endforeach()

  # Ubuntu 导出的 clangdMain 目标会链接远程索引代码，因此还要求这些
  # 系统库。全部静默探测，任一缺失时跳过整个自定义工具目录。
  foreach(DEP_LIBRARY protobuf gpr grpc grpc++ absl_synchronization)
    string(MAKE_C_IDENTIFIER "${DEP_LIBRARY}" DEP_ID)
    find_library(UCO_CLANG_TIDY_${DEP_ID}_LIBRARY NAMES ${DEP_LIBRARY})
    if(NOT UCO_CLANG_TIDY_${DEP_ID}_LIBRARY)
      set(UCO_CLANG_TIDY_DEPS_FOUND FALSE)
    endif()
  endforeach()

  if(UCO_CLANG_TIDY_DEPS_FOUND)
    add_subdirectory(tools/clang-tidy)
  else()
    # 开关即使为 ON，依赖缺失也只提示，不中止项目配置或普通目标构建。
    message(STATUS
            "clang-tidy/clangd dependencies not found; custom tooling disabled")
  endif()
endif()

find_package(Protobuf REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
set(GENERATED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/gen)
file(MAKE_DIRECTORY ${GENERATED_DIR}) # gen 不存在时创建 (全新环境直接构建).

option(USE_JEMALLOC "Use jemalloc" OFF)

# 生成结果经 SRCS_VAR/HDRS_VAR 回传父作用域 (供 add_executable 引用).
function(uco_proto_generate PROTO_ROOT OUT_DIR SRCS_VAR HDRS_VAR)
    # 逐文件 protoc, 输出镜像 proto 相对目录 (proto/dao/user.proto -> gen/dao/user.pb.*);
    # protobuf_generate_cpp 按基名平铺输出, 各层同名 user.proto 会互相覆盖.
    file(MAKE_DIRECTORY ${OUT_DIR})
    set(${SRCS_VAR} "")
    set(${HDRS_VAR} "")
    file(GLOB_RECURSE PROTOS "${PROTO_ROOT}/*.proto")
    foreach(FIL ${PROTOS})
        file(RELATIVE_PATH REL_FIL ${PROTO_ROOT} ${FIL})
        string(REGEX REPLACE "\\.proto$" "" STEM ${REL_FIL})
        list(APPEND ${SRCS_VAR} ${OUT_DIR}/${STEM}.pb.cc)
        list(APPEND ${HDRS_VAR} ${OUT_DIR}/${STEM}.pb.h)
        add_custom_command(
            OUTPUT ${OUT_DIR}/${STEM}.pb.cc ${OUT_DIR}/${STEM}.pb.h
            COMMAND protobuf::protoc --cpp_out ${OUT_DIR}
                    -I ${PROTO_ROOT} ${FIL}
            DEPENDS ${FIL} protobuf::protoc
            COMMENT "Running cpp protocol buffer compiler on ${FIL}"
            VERBATIM)
    endforeach()
    set(${SRCS_VAR} ${${SRCS_VAR}} PARENT_SCOPE)
    set(${HDRS_VAR} ${${HDRS_VAR}} PARENT_SCOPE)
endfunction()

uco_proto_generate(${CMAKE_CURRENT_SOURCE_DIR} ${GENERATED_DIR}
                   PROTO_SRCS PROTO_HDRS)

include_directories(
    ${Protobuf_INCLUDE_DIRS}
    ${GENERATED_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_library(ulog core/ulogshm.cpp core/ulog.cpp)
target_link_libraries(ulog PRIVATE ${Protobuf_LIBRARIES})

add_library(uhook SHARED core/uhook.cpp)

# core 公共源: 全量 glob 后剔除归属其他目标的文件
# (uhook.cpp→uhook, ulog*.cpp→ulog/ulogcsm, usql.cpp→条件加入).
file(GLOB_RECURSE CORE_UCO_SRC CONFIGURE_DEPENDS "core/*.cpp")
list(FILTER CORE_UCO_SRC EXCLUDE REGEX
     "core/(uhook|ulog|ulogshm|ulogcsm|usql)\\.cpp$")
add_library(uco ${CORE_UCO_SRC})
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

# http 框架层: server 与未来的业务方共同链接.
file(GLOB_RECURSE SRC_HTTP CONFIGURE_DEPENDS "http/*.cpp")
add_library(uhttp ${SRC_HTTP})
target_link_libraries(uhttp PUBLIC ulog uco uring ${Protobuf_LIBRARIES})

# 业务层 (controller/service/dao + main).
file(GLOB_RECURSE SRC_WEBSERVER CONFIGURE_DEPENDS "server/*.cpp")

add_executable(
    server
    ${PROTO_SRCS}
    ${PROTO_HDRS}
    ${SRC_WEBSERVER}
)
target_link_libraries(server PRIVATE uhttp)
# ---- bcrypt (libxcrypt crypt_r, service 层密码校验) ----
find_library(CRYPT_LIBRARY crypt)
if(CRYPT_LIBRARY)
  target_link_libraries(server PRIVATE ${CRYPT_LIBRARY})
else()
  message(FATAL_ERROR "libcrypt not found: service 层 bcrypt 依赖 libxcrypt")
endif()
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

add_executable(test_threadpool tests/test_threadpool.cpp)
target_link_libraries(test_threadpool PRIVATE ulog uco uring)

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
