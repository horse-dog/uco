#!/bin/bash
# ==============================================================================
# UCO 一键环境部署脚本
#
# 适用系统 : Ubuntu 22.04 / 24.04, Debian 12 及以上 (apt 系发行版)
# 用法     : bash environment.sh
# 说明     : 幂等设计, 可重复执行; 已安装的包会被 apt 自动跳过.
# ==============================================================================

set -euo pipefail

# ---------- 前置检查 -----------------------------------------------------------

if [[ $EUID -ne 0 ]]; then
    echo "[uco] 需要 root 权限, 使用 sudo 重新执行本脚本..." >&2
    exec sudo bash "$0" "$@"
fi

if ! command -v apt-get >/dev/null 2>&1; then
    echo "[uco] 错误: 本脚本仅支持 apt 系发行版 (Ubuntu/Debian)." >&2
    exit 1
fi

KVER_MAJOR=$(uname -r | cut -d. -f1)
KVER_MINOR=$(uname -r | cut -d. -f2)
KVER=$((10#${KVER_MAJOR} * 100 + 10#${KVER_MINOR}))
if [[ ${KVER} -lt 601 ]]; then
    echo "[uco] 警告: 内核版本 $(uname -r) 偏低, io_uring 建议使用 6.1+ (推荐 6.8+)." >&2
fi

# ---------- 必需依赖 -----------------------------------------------------------
# CMake 配置强依赖以下各项, 缺失任一则构建失败.

echo "[uco] [1/2] 安装构建工具链与必需依赖..."
apt-get update
apt-get install -y \
    build-essential \
    cmake \
    liburing-dev \
    libprotobuf-dev \
    protobuf-compiler \
    libssl-dev \
    libcrypt-dev \
    libmysqlclient-dev

# 说明:
#   build-essential     g++ >= 13 (C++20 协程) 与 make
#   liburing-dev        io_uring 异步 I/O
#   libprotobuf-dev     Protocol Buffers 运行库
#   protobuf-compiler   protoc 代码生成
#   libssl-dev          OpenSSL (Crypto 组件: HMAC 等)
#   libcrypt-dev        libcrypt/libxcrypt (service 层 bcrypt 密码校验)
#   libmysqlclient-dev  Oracle 官方 MySQL 客户端 (usql)

# ---------- 可选依赖 -----------------------------------------------------------
# 任一缺失时 CMake 会自动降级或跳过对应功能, 不影响主体构建:
#   libjemalloc-dev  USE_JEMALLOC 默认 ON; 缺失时自动回退默认分配器
#   wrk              HTTP 压测工具 (见 README 性能测试章节)

echo "[uco] [2/2] 安装可选依赖 (jemalloc / wrk)..."
apt-get install -y \
    libjemalloc-dev \
    wrk \
    || echo "[uco] 提示: 部分可选依赖安装失败, 已跳过 (主体构建不受影响)."

# ---------- 完成摘要 -----------------------------------------------------------

echo
echo "[uco] 依赖安装完成, 关键组件版本:"
echo "  $(g++ --version | head -1)"
echo "  $(cmake --version | head -1)"
echo "  protoc        $(protoc --version)"
echo "  kernel        $(uname -r)"
echo
echo "[uco] 下一步:"
echo "  mkdir build && cd build && cmake .. && make -j\$(nproc)"
