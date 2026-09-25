<div align="center">

# UCO

**基于 Linux io_uring 与 C++20 协程的高性能 Web 服务器**

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![io_uring](https://img.shields.io/badge/IO-io_uring-orange)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey)
![QPS](https://img.shields.io/badge/QPS-121k%2B-brightgreen)
![License](https://img.shields.io/badge/license-MIT-green)

</div>

---

## 目录

- [简介](#简介)
- [快速开始](#快速开始)
- [环境要求](#环境要求)
- [配置说明](#配置说明)
- [性能测试](#性能测试)
- [项目结构](#项目结构)
- [调试工具](#调试工具)
- [许可证](#许可证)

## 简介

UCO（**U**ltra **CO**routine）是一台基于 Linux `io_uring` 与 C++20 无栈协程的 HTTP 服务器。每个 worker 线程拥有独立的 io_uring 实例，全链路（accept、读写、sendfile、MySQL/Redis 客户端）协程化异步，无阻塞、无线程级回调地狱。

**核心特性：**

- `io_uring` 驱动 — 全异步 I/O，包括 `sendfile` 与静态资源 LRU fd 缓存
- C++20 无栈协程 — `co_await` 风格业务代码，内存开销远低于线程
- 完整框架语义 — 路由前缀树、中间件链、Session、限流、CSRF 防护
- 协程化存储客户端 — 异步 MySQL（usql）与 Redis（uredis）
- 共享内存日志 — 独立消费者进程落盘，主路径零 I/O
- jemalloc 自动检测 — 系统存在则启用，不存在自动回退默认分配器

## 快速开始

```bash
# 1. 安装依赖 (Ubuntu 22.04/24.04, Debian 12+)
bash environment.sh

# 2. 一键启动 (自动就绪 MySQL/Redis、初始化业务库表、按需构建)
bash run.sh
```

`run.sh` 会自动完成：安装并启动 MySQL/Redis → 幂等创建 `webserver` 库与 `user` 表（`sql/user.sql`）→ 检测构建产物（缺失则编译）→ 前台运行 `./build/server`。

服务启动后访问 `http://<host>:8080/`。

<details>
<summary>手动构建与运行（不用 run.sh）</summary>

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)

# 从项目根目录运行, 静态资源 res/ 才能正确解析
cd ..
./build/server

# 可选: 日志消费者 (logging.mode: 1 文件模式需要)
./build/ulogcsm -o <logdir>
```
</details>

## 环境要求

**系统**：Ubuntu 22.04 / 24.04、Debian 12+（apt 系）；Linux 内核 ≥ 6.1，推荐 6.8+。

**编译依赖**（`bash environment.sh` 一键安装）：

| 分类 | 依赖 | 包名 | 说明 |
|------|------|------|------|
| 必需 | g++ ≥ 13 | `build-essential` | C++20 协程 |
| 必需 | CMake ≥ 3.20 | `cmake` | 构建系统 |
| 必需 | liburing | `liburing-dev` | io_uring 用户态库 |
| 必需 | Protobuf | `libprotobuf-dev` + `protobuf-compiler` | 序列化与代码生成 |
| 必需 | OpenSSL | `libssl-dev` | Crypto 组件（HMAC 等） |
| 必需 | libcrypt | `libcrypt-dev` | bcrypt 密码校验 |
| 必需 | MySQL 客户端 | `libmysqlclient-dev` | Oracle 官方，异步 usql |
| 可选 | jemalloc | `libjemalloc-dev` | **自动检测**：存在则链接，缺失则回退默认分配器 |
| 可选 | LLVM/Clang 18 | `clang-18` 等 | 仅定制 clang-tidy 插件需要，缺失自动跳过 |

**运行时依赖**（`bash run.sh` 自动就绪）：

| 服务 | 用途 | 说明 |
|------|------|------|
| MySQL | demo 登录/注册 | 凭据见 `server/config.yaml` 的 `mysql` 段 |
| Redis | Session 与限流 | 默认 `127.0.0.1:6379` |

## 配置说明

配置文件：`server/config.yaml`（路径编译期内嵌，可用 `-c <path>` 覆盖）。

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `http.port` | `8080` | 监听端口 |
| `http.worker_threads` | `4` | worker 线程数（每线程独立 io_uring 实例） |
| `http.keepalive_max_requests` | `0` | 单连接最大请求数，0 为不限制 |
| `http.keepalive_timeout_sec` | `30` | 长连接空闲超时 |
| `logging.level` | `2` | 0=DEBUG 1=INFO 2=WARN 3=ERROR 4=FATAL |
| `logging.mode` | `0` | 0=CONSOLE 1=FILE（需配合 ulogcsm） |
| `mysql.*` / `redis.*` | — | 连接池地址、容量与超时 |
| `rate_limit.*` | — | IP / Session / 账号三级限流窗口 |
| `session.*` | — | Cookie 密钥、有效期、TTL 刷新策略 |

## 性能测试

### 测试环境

| 项目 | 信息 |
|------|------|
| OS | Ubuntu 24.04.4 LTS (Noble Numbat) |
| 内核 | 6.8.0-110-generic |
| CPU | AMD EPYC 7K62 48-Core Processor / 4 核 / 1 线程/核 |
| 内存 | 3.6 GB（总）/ 1.8 GB（可用） |
| 磁盘 | 40 GB（总）/ 28 GB（可用） |

### Hello 接口（轻量级响应）

```bash
wrk -t4 -c500 -d10s http://127.0.0.1:8080/hello
```

```
Running 10s test @ http://127.0.0.1:8080/hello
  4 threads and 500 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     3.91ms    1.34ms  54.86ms   72.09%
    Req/Sec    30.64k     1.33k   36.34k   75.50%
  1222690 requests in 10.05s, 192.40MB read
Requests/sec: 121645.42
Transfer/sec:     19.14MB
```

**QPS: ~121,645** | 平均延迟: **3.91ms**

### 多接口混合测试

```bash
cd tests && wrk -t4 -c500 -d10s -s urls.lua http://127.0.0.1:8080
```

```
Running 10s test @ http://127.0.0.1:8080
  4 threads and 500 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     7.33ms    2.75ms  24.58ms   70.14%
    Req/Sec    16.77k   661.23    20.51k   75.75%
  669649 requests in 10.05s, 1.82GB read
Requests/sec: 66642.15
Transfer/sec:     185.56MB
```

**QPS: ~66,642** | 平均延迟: **7.33ms**

### 静态文件（/index）

```
Running 10s test @ http://127.0.0.1:8080/index
  4 threads and 500 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     5.46ms    1.79ms  20.08ms   73.66%
    Req/Sec    22.40k     1.73k   34.90k   84.75%
  894251 requests in 10.05s, 2.73GB read
Requests/sec: 88978.76
Transfer/sec:     278.16MB
```

**QPS: ~88,979** | 平均延迟: **5.46ms**

### 横向对比

> 同环境：VMware Ubuntu 24.04 | 4 线程 / 500 并发连接 / 10 秒 | `/index` 接口

| 框架 | QPS | 平均延迟 | 吞吐量 | 备注 |
|------|-----|---------|--------|------|
| **UCO** | **88,979** | **5.46ms** | **278.16 MB/s** | 本项目 |
| Go + Gin | 60,139 | 22.33ms | 191.21 MB/s | `go run main.go` |
| [WebServer (C++)](https://github.com/markparticle/WebServer) | 16,874 | 18.46ms | 52.62 MB/s | 有 26 个超时 |

UCO 在 QPS 上领先 Go+Gin 约 **48%**，领先 WebServer(C++) 约 **428%**，同时保持最低的平均延迟。

## 项目结构

```
uco/
├── core/           # 协程核心与基础设施 (uco 协程、usync 同步原语、ulog 日志、uredis、usql)
├── http/           # HTTP 框架层 (httpserver 路由/中间件、httpconnection、session、ratelimit、csrf)
├── server/         # 业务层 (main.cpp、controller/service/dao、config.yaml)
├── demo/           # Protobuf 业务示例定义
├── tests/          # 单元测试与压测脚本 (wrk.sh、urls.lua、gin 对比示例)
├── res/            # 静态资源 (html/css/js/images/video)
├── sql/            # 数据库初始化脚本 (user.sql)
├── tools/          # 定制 clang-tidy 插件 (可选构建)
├── environment.sh  # 一键环境部署
├── run.sh          # 一键启动 (依赖服务 + 库表初始化 + 构建 + 运行)
├── CMakeLists.txt  # 构建配置
└── README.md       # 本文档
```

## 调试工具

<details>
<summary>Valgrind 内存泄漏检测</summary>

```bash
wget -q https://sourceware.org/pub/valgrind/valgrind-3.24.0.tar.bz2
tar -xjf valgrind-3.24.0.tar.bz2
cd valgrind-3.24.0
./configure
make -j$(nproc)
sudo make install

cd build && valgrind ./server
```
</details>

<details>
<summary>系统信息快速查看</summary>

```bash
echo "=== OS ===" && cat /etc/os-release | grep -E "NAME|VERSION" && \
echo "=== Kernel ===" && uname -r && \
echo "=== CPU ===" && lscpu | grep -E "Model name|CPU\(s\)|Thread|Core|Socket" && \
echo "=== Memory ===" && free -h | head -2 && \
echo "=== Disk ===" && df -h / | tail -1
```
</details>

## 许可证

[MIT License](LICENSE)
