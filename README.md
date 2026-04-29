# 🚀 UCO — 基于 io_uring 的无栈协程高性能 HTTP 服务器

<p align="center">
  <strong>基于 Linux io_uring 与 C++20 协程的轻量级、高并发 Web 服务器</strong>
</p>
<p align="center">
  <strong>本文档由 AI 生成</strong>
</p>

---

## ✨ 特性

- **io_uring 驱动**：充分利用 Linux 内核 6.8+ 的异步 I/O 能力
- **无栈协程（Stackless Coroutine）**：C++20 协程实现，内存开销极低
- **高性能**：单机 QPS 可达 **12 万+**（hello 接口）、**8.9 万+**（静态文件）
- **Keep-Alive 支持**：可配置长连接复用，大幅提升吞吐量
- **日志系统**：内置异步日志消费者，支持高性能日志写入
- **可选 jemalloc**：支持接入 jemalloc 分配器以进一步优化内存性能

---

## 🔧 环境要求

| 依赖 | 版本要求 | 说明 |
|------|---------|------|
| Linux 内核 | ≥ 6.8 | 提供 io_uring 支持 |
| g++ | ≥ 13 | 需要 C++20 标准 |
| CMake | ≥ 3.20 | 构建系统 |
| liburing-dev | 最新 | io_uring 库 |
| libprotobuf-dev | 3.x | Protocol Buffers |
| protobuf-compiler | 3.x | protoc 编译器 |
| libjemalloc-dev | 可选 | 高性能内存分配器 |

### 一键安装依赖

```bash
bash environment.sh
```

### 安装压测工具（可选）：sudo apt install wrk

---

## 📦 构建与运行

```bash
# 构建
mkdir build && cd build
cmake ..
make -j$(nproc)

# 启动日志消费者（可选，写日志到文件的模式下需要开启）
./ulogcsm -o <logdir>

# 启动服务端
./server
```

默认监听端口：**8080**

### 配置说明

在 `main.cpp` 中调整初始化参数：

```cpp
httpserver.Init(8080, 4, 1000000000, 30);
//              端口  线程数  最大连接数   超时秒数
```

> ⚠️ **压测建议**：建议使用 `httpserver.Init(8080, 4, 1000000000, 30);` 以避免长连接超过服务次数被断开，否则可能出现 **read error**。

### 启用 jemalloc（可选）

jemalloc 为可选项，按需开启即可。


编辑 `CMakeLists.txt`：

```cmake
# 将 OFF 改为 ON（需要时开启）
option(USE_JEMALLOC "Use jemalloc" ON)
```

重新构建即可生效。

---

## 🧪 性能测试

### 测试环境

| 项目 | 信息 |
|------|------|
| OS | Ubuntu 24.04.4 LTS (Noble Numbat) |
| 内核 | 6.8.0-110-generic |
| CPU | AMD EPYC 7K62 48-Core Processor / 4 核 / 1 线程/核 |
| 内存 | 3.6 GB（总）/ 1.8 GB（可用） |
| 磁盘 | 40 GB（总）/ 28 GB（可用） |

### UCO 基准测试

#### Hello 接口（轻量级响应）

```bash
wrk -t4 -c500 -d10s http://127.0.0.1:8080/hello
```

```
Running 10s test @ http://127.0.0.1:8080/hello
  4 threads and 500 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     3.91ms    1.34ms  54.86ms   72.09%
    Req/Sec    30.64k     1.33k   36.34k    75.50%
  1222690 requests in 10.05s, 192.40MB read
Requests/sec: 121645.42
Transfer/sec:     19.14MB
```

📊 **QPS: ~121,645** | 平均延迟: **3.91ms**

#### 多接口混合测试

```bash
cd tests && wrk -t4 -c500 -d10s -s urls.lua http://127.0.0.1:8080
```

```
Running 10s test @ http://127.0.0.1:8080
  4 threads and 500 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     7.33ms    2.75ms  24.58ms   70.14%
    Req/Sec    16.77k   661.23    20.51k    75.75%
  669649 requests in 10.05s, 1.82GB read
Requests/sec:  66642.15
Transfer/sec:    185.56MB
```

📊 **QPS: ~66,642** | 平均延迟: **7.33ms**

#### 静态文件（/index）

```
Running 10s test @ http://127.0.0.1:8080/index
  4 threads and 500 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     5.46ms    1.79ms  20.08ms   73.66%
    Req/Sec    22.40k     1.73k   34.90k    84.75%
  894251 requests in 10.05s, 2.73GB read
Requests/sec:  88978.76
Transfer/sec:    278.16MB
```

📊 **QPS: ~88,979** | 平均延迟: **5.46ms**

---

## 📊 横向对比

> 测试环境：VMware Ubuntu 24.04 | 4 线程 / 500 并发连接 / 10 秒 | `/index` 接口

| 框架 | QPS | 平均延迟 | 吞吐量 | 备注 |
|------|-----|---------|--------|------|
| **UCO** ⭐ | **88,979** | **5.46ms** | **278.16 MB/s** | 本项目 |
| Go + Gin | 60,139 | 22.33ms | 191.21 MB/s | `go run main.go` |
| [WebServer (C++)](https://github.com/markparticle/WebServer) | 16,874 | 18.46ms | 52.62 MB/s | 有 26 个超时 |

**结论**：UCO 在 QPS 上领先 Go+Gin 约 **48%**，领先 WebServer(C++) 约 **428%**，同时保持最低的平均延迟。

---

## 🐛 调试工具

### Valgrind 内存泄漏检测（可选）

```bash
# 安装 Valgrind
wget -q https://sourceware.org/pub/valgrind/valgrind-3.24.0.tar.bz2
tar -xjf valgrind-3.24.0.tar.bz2
cd valgrind-3.24.0
./configure
make -j$(nproc)
sudo make install

# 使用 Valgrind 运行
cd build && valgrind ./server
```

### 系统信息快速查看

```bash
echo "=== OS ===" && cat /etc/os-release | grep -E "NAME|VERSION" && \
echo "=== Kernel ===" && uname -r && \
echo "=== CPU ===" && lscpu | grep -E "Model name|CPU\(s\)|Thread|Core|Socket" && \
echo "=== Memory ===" && free -h | head -2 && \
echo "=== Disk ===" && df -h / | tail -1
```

---

## 📁 项目结构

```
uco/
├── core/          # 协程核心实现 (uco.h/cpp, usync.h)
├── src/           # HTTP 服务端模块 (httpserver, httpresponce)
├── common/        # 通用工具 (allocator 等)
├── proto/         # Protobuf 定义
├── resources/     # 静态资源 (HTML/JS/图片)
├── test/          # 测试用例及基准测试脚本
├── gin_demo/      # Go/Gin 对比示例
├── hook_exit/     # Hook 相关
├── main.cpp       # 入口文件
├── CMakeLists.txt # 构建配置
└── ReadMe.md      # 本文档
```

---

## 📜 许可证

本项目基于 [MIT License](LICENSE) 开源。

---

<p align="center">
  <sub>Built with ❤️ using C++20 & io_uring</sub>
</p>
