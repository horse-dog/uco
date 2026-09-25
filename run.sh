#!/bin/bash
# ==============================================================================
# UCO 一键启动脚本
#
# 功能: 安装并启动 MySQL / Redis -> 业务库表幂等初始化 -> 按需构建 -> 运行 server
# 用法: bash run.sh          (从项目任意位置执行均可, 前台运行, Ctrl+C 退出)
# ==============================================================================

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${PROJECT_DIR}"

MYSQL_USER="root"          # 与 server/config.yaml 中 mysql 段保持一致
MYSQL_PASS="123456"
MYSQL_DB="webserver"
SQL_FILE="${PROJECT_DIR}/sql/user.sql"

log()  { echo "[uco] $*"; }
fail() { echo "[uco] 错误: $*" >&2; exit 1; }

# ---------- 1. MySQL / Redis 安装与启动 ----------------------------------------

ensure_service()
{
    local pkg="$1" svc="$2"
    if ! dpkg -s "${pkg}" >/dev/null 2>&1; then
        log "安装 ${pkg} ..."
        sudo apt-get install -y "${pkg}" >/dev/null \
            || fail "无法安装 ${pkg}, 请手动安装后重试"
    fi
    if ! systemctl is-active --quiet "${svc}"; then
        log "启动服务 ${svc} ..."
        sudo systemctl start "${svc}" || sudo service "${svc}" start \
            || fail "无法启动 ${svc}"
    fi
}

ensure_service mysql-server mysql
ensure_service redis-server redis-server

# ---------- 2. Redis 连通性检查 --------------------------------------------------

redis-cli ping 2>/dev/null | grep -q PONG || fail "Redis 未就绪 (redis-cli ping 无响应)"

# ---------- 3. MySQL 凭据与业务库表初始化 (幂等) ----------------------------------

MYSQL_CMD=(mysql -h127.0.0.1 -P3306 -u"${MYSQL_USER}")

mysql_ready()
{
    MYSQL_PWD="${MYSQL_PASS}" "${MYSQL_CMD[@]}" -e "SELECT 1" >/dev/null 2>&1
}

if ! mysql_ready; then
    # Ubuntu 默认 root 为 auth_socket 免密登录, 尝试按 config.yaml 的密码初始化.
    log "MySQL TCP 凭据未配置, 尝试将 root 密码初始化为 config.yaml 中的默认值..."
    if sudo mysql -uroot -e \
        "ALTER USER 'root'@'localhost' IDENTIFIED WITH mysql_native_password BY '${MYSQL_PASS}'; FLUSH PRIVILEGES;" \
        >/dev/null 2>&1; then
        mysql_ready || fail "root 密码已设置但 TCP 连接仍失败, 请检查 config.yaml"
    else
        fail "无法连接 MySQL: 请将 server/config.yaml 中 mysql 段的密码改为实际 root 密码后重试"
    fi
fi
log "MySQL 连接正常 (root@127.0.0.1)"

# 业务库: 不存在则创建.
if ! MYSQL_PWD="${MYSQL_PASS}" "${MYSQL_CMD[@]}" -e "USE \`${MYSQL_DB}\`" >/dev/null 2>&1; then
    log "创建业务数据库 ${MYSQL_DB} ..."
    MYSQL_PWD="${MYSQL_PASS}" "${MYSQL_CMD[@]}" \
        -e "CREATE DATABASE \`${MYSQL_DB}\` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
fi

# 业务表: 不存在则导入 (幂等, 已存在则跳过).
if MYSQL_PWD="${MYSQL_PASS}" "${MYSQL_CMD[@]}" "${MYSQL_DB}" \
       -e "SHOW TABLES LIKE 'user'" 2>/dev/null | grep -q user; then
    log "业务表 ${MYSQL_DB}.user 已存在, 跳过初始化"
else
    log "导入业务表结构 ${SQL_FILE} ..."
    MYSQL_PWD="${MYSQL_PASS}" "${MYSQL_CMD[@]}" "${MYSQL_DB}" < "${SQL_FILE}"
fi

# ---------- 4. 按需构建 -----------------------------------------------------------

if [[ ! -x "${PROJECT_DIR}/build/server" ]]; then
    log "未发现构建产物, 开始编译 (首次构建约需数分钟) ..."
    if [[ ! -d "${PROJECT_DIR}/build" ]]; then
        mkdir "${PROJECT_DIR}/build"
    fi
    cmake -S "${PROJECT_DIR}" -B "${PROJECT_DIR}/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "${PROJECT_DIR}/build" -j"$(nproc)" >/dev/null \
        || fail "编译失败, 请检查构建环境 (bash environment.sh)"
fi

# ---------- 5. 运行 ----------------------------------------------------------------

log "启动 server (端口 8080, Ctrl+C 退出) ..."
log "提示: 需查看 HTTP 外网访问时请确认云厂商安全组已放行 8080"
cd "${PROJECT_DIR}"
exec ./build/server
