# HRUFT Pro v4.0.0

![C++](https://img.shields.io/badge/C%2B%2B-17-blue)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey)
![License](https://img.shields.io/badge/license-MIT-blue)

基于 UDT4 的可靠 UDP 大文件传输工具。流式 BLAKE3 校验、流水线读写、传输结束后输出 JSON 网络诊断报告。

> **v4.0.0 是破坏性更新**：控制消息改为带长度前缀的定长帧，与 v3.x **不互通**，收发两端必须同时升级。

---

## ✨ 主要特性

- **分帧协议**：所有控制消息（头/哈希/完成/ACK/报告）都带 4 字节长度前缀，不再依赖「一次 recv 恰好读到一条消息」
- **三段流水线**：读盘 / BLAKE3 计算 / 网络收发并行，避免三者互相等待
- **BLAKE3 流式校验**：边传边算，256 位摘要，传输结束双向比对
- **接收窗口可真正放开**：同时设置 `UDT_FC`（飞行窗口，单位**包**）与 `UDT_RCVBUF`，否则窗口会被 UDT 默认的 25600 包截断
- **先落 `.part` 再改名**：哈希不通过就删除，磁盘上不会出现损坏文件
- **路径穿越防护**：对端文件名只保留纯文件名，`../../x`、`/etc/passwd` 一律降级
- **中文路径**：Windows 走 UTF-8 ↔ UTF-16 全程转换，Linux 直接使用 UTF-8
- **完整错误检查**：所有 `setsockopt` / `listen` / `recv` / `send` 返回值均检查，失败即报错而非静默继续
- **不阻塞**：ACK 等待带 30s 超时，对端崩溃不会让发送端永久挂起

---

## 📦 系统要求

### 操作系统
- Windows 7/8/10/11
- Linux（Ubuntu 18.04+ / Debian 9+ / CentOS 7+ 等）

### 编译器
- GCC 9+ / Clang 10+（Linux）
- MinGW-w64 8+ 或 Visual Studio 2017+（Windows）
- 需要 C++17

### 依赖

| 依赖 | 说明 |
|------|------|
| **UDT4** | **需自行提供**，见下方「获取 UDT4」 |
| BLAKE3 | `lib/` 下已附带 linux / arm32 / arm64 / win 四套预编译库 |
| nlohmann/json | `lib/json.hpp` 已附带（v3.12.0） |

---

## 🚀 快速开始

### 1. 获取 UDT4

```bash
git clone https://github.com/JinBiLianShao/udt4.git ../udt4
```

> **必须是源码树，不能是二进制安装**。HRUFT 的 `--no-cc` 需要 `ccc.h`（自定义拥塞控制），
> 而该头文件通常不在 `make install` 的产物里。CMake 会自动探测 `../udt4`，也可显式指定：
> `-DUDT_ROOT=/path/to/udt4`

### 2. 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

CMake 会自动把 UDT4 作为子项目编译（`ccc.cpp`、`cache.cpp` 缺任一个都会链接失败，子项目方式可避免）。

### 3. 发送 / 接收

```bash
# 发送
./build/hruft send <目标IP> <端口> <文件路径> [选项]

# 接收（savepath 可以是目录，也可以是完整文件名）
./build/hruft recv <监听端口> <保存路径> [选项]
```

---

## 📖 使用指南

### 发送端

```bash
hruft send <ip> <port> <filepath> [options]
```

| 参数 | 描述 | 默认值 | 必填 |
|------|------|--------|------|
| `ip` | 目标 IP（仅 IPv4，不支持域名/NAT 穿透） | - | 是 |
| `port` | 目标端口（1-65535） | - | 是 |
| `filepath` | 要发送的**普通文件**路径 | - | 是 |
| `--mss` | 最大分段大小，536-8900 | 1500 | 否 |
| `--window` | 窗口大小（字节），最小 64KB，上限 256MB | 10MB | 否 |
| `--detailed` | 每秒输出瞬时速率与网络统计 | false | 否 |
| `--no-cc` | 关闭拥塞控制（仅限直连链路，见下方警告） | false | 否 |

### 接收端

```bash
hruft recv <port> <savepath> [options]
```

| 参数 | 描述 | 默认值 | 必填 |
|------|------|--------|------|
| `port` | 监听端口（1-65535） | - | 是 |
| `savepath` | 目录，或完整文件名 | - | 是 |
| `--mss` | 同发送端。**接收端设置才决定实际 MSS**（见下） | 1500 | 否 |
| `--window` | 同发送端。**接收端设置才决定实际接收窗口** | 10MB | 否 |
| `--max-size` | 拒绝大于该字节数的文件 | 不限 | 否 |
| `--detailed` | 每秒输出瞬时速率与网络统计 | false | 否 |
| `--no-cc` | 关闭拥塞控制 | false | 否 |

### ⚠️ 参数由接收端决定，不是发送端

这一点容易踩坑，务必理解：

- **MSS**：UDT 握手时取双方较小值（`core.cpp: connect()`），所以任一端设小都会生效
- **接收窗口**：由**接收端**的 `UDT_RCVBUF` / `UDT_FC` 决定，发送端只是把接收端通告的飞行窗口当上限

因此 `hruft send ... --window 128MB` 在接收端仍是默认 10MB 时**毫无效果**。要提速必须两端一起调，
或干脆只在接收端调大。

---

## 🔄 协议说明（v4）

### 帧格式

```
+----------------+----------+------------------+
| u32 length (BE) | u8 type  | payload (length-1) |
+----------------+----------+------------------+
```

`length` 含类型字节本身，上限 64MB。类型：

| type | 名称 | payload |
|------|------|---------|
| 1 | HEADER | `ProtocolHeader`(24B) + 文件名(UTF-8，无终止符) |
| 2 | HASH | 32 字节 BLAKE3 摘要 |
| 3 | DONE | 空 |
| 4 | ACK | 1 字节状态（0=OK，1=哈希不符，2=中止） |
| 5 | REPORT | UTF-8 JSON |

### 协议头

```cpp
#pragma pack(push, 1)
struct ProtocolHeader {
    uint32_t magic;        // 0x48525034 "HRP4"
    uint16_t version;      // 4
    uint16_t flags;        // bit0: 发送端启用了 --no-cc
    uint64_t file_size;
    uint32_t filename_len;
    uint32_t reserved;
};  // 24 字节，大端
#pragma pack(pop)
```

### 传输时序

```
发送端                                    接收端
  |---- HEADER(头+文件名) --------------->|
  |---- 文件数据（裸字节流，无帧）------->|
  |---- HASH(32B) ---------------------->|
  |---- DONE --------------------------->|
  |<--- ACK(1B) ------------------------|
  |<--- REPORT(JSON) -------------------|
```

文件数据本身不加帧（避免每 4MB 多一次小包），长度由 `file_size` 精确界定。

---

## 🔧 CMake 选项

| 选项 | 说明 | 默认 |
|------|------|------|
| `CMAKE_BUILD_TYPE` | `Release` / `Debug` | Release |
| `UDT_ROOT` | UDT4 源码树或安装前缀 | 自动探测 `../udt4` |
| `TARGET_OS` | `linux` / `windows` | linux |
| `COMPILER_TARGET` | `x86` / `arm32` / `arm64` | x86 |
| `ENABLE_OPENMP` | 启用 OpenMP | OFF |

```bash
# 交叉编译 arm64（需装 aarch64-linux-gnu-gcc/g++）
cmake -S . -B build-arm64 -DCOMPILER_TARGET=arm64 -DCMAKE_BUILD_TYPE=Release
cmake --build build-arm64 -j
```

---

## 📊 输出报告

接收端在传输结束后生成 JSON 报告，同时打印到本地并发回发送端：

```json
{
  "meta": {
    "filename": "large_file.iso",
    "filepath": "/data/large_file.iso",
    "filesize": 1073741824,
    "filesize_human": "1.00 GB",
    "status": "success",
    "hash_match": true,
    "local_hash": "a7b...f2e",
    "remote_hash": "a7b...f2e",
    "duration_sec": 42.5,
    "avg_speed_mbps": 201.8,
    "avg_speed_mbs": 25.2,
    "congestion_control_disabled": false,
    "protocol_version": 4
  },
  "throughput": { "avg_mbps": 201.8, "inst_mbps": 215.3, "est_bandwidth_mbps": 950.0 },
  "latency": { "rtt_ms": 28.5 },
  "reliability": {
    "pkt_sent": 850234, "pkt_recv": 850200,
    "pkt_loss_sent": 12, "pkt_loss_recv": 10,
    "retrans_total": 34, "retrans_ratio": 0.00004
  },
  "congestion": { "window_flow": 91180, "window_cong": 16, "flight_size": 4096 },
  "buffer_health": { "snd_buf_avail_bytes": 16777216, "rcv_buf_avail_bytes": 8388608 },
  "analysis": {
    "network_health": "excellent",
    "bdp_bytes_est": 3325000,
    "advice": ["网络质量: 优秀。如果支持巨型帧，可尝试 --mss 8900。"]
  }
}
```

### 网络质量等级

| 等级 | 触发条件 |
|------|----------|
| `excellent` | 重传率 < 0.1% 且无窗口告警 |
| `good` | 重传率 0.1% - 1% |
| `suboptimal` | 配置窗口小于 BDP |
| `network_lossy` | 重传率 > 1% |
| `unknown` | 统计数据缺失 |

---

## 🔍 故障排除

### 编译期

**`UDT4 headers not found`**
→ 没克隆 UDT4，或路径不对。`git clone https://github.com/JinBiLianShao/udt4.git ../udt4`，或 `-DUDT_ROOT=`。

**`missing ccc.h`**
→ 你指向的是二进制安装目录。`--no-cc` 需要 UDT4 **源码树**里的 `ccc.h`。

**`undefined reference to CCC::CCC()` / `CInfoBlock::getKey()`**
→ 你用的 UDT4 版本 `CMakeLists.txt` 漏了 `src/ccc.cpp` 和 `src/cache.cpp`。
  这两个文件分别提供自定义拥塞控制和连接信息缓存，缺任一个都会链接失败。已在上游修复；
  若用旧版请手动补进 `UDT_SOURCES`。

### 运行期

**传输速度远低于预期**
1. 先看报告的 `congestion.window_flow`。若它停在 `25600` 左右，说明 `UDT_FC` 没放开——
   MSS 1500 时这就是约 37MB 的硬上限，`--window` 再大也无效。
2. 确认 `--window` 是**在接收端**调的。
3. 高延迟链路：`window` 应 ≥ 带宽 × RTT，否则看 `analysis.advice` 里的 BDP 提示。

**哈希校验失败**
接收端会删除 `.part` 临时文件并返回非 0，磁盘上不会留下损坏文件。检查链路稳定性后重传。

**接收端报 `Unsafe filename`**
对端发来的文件名含路径分隔符，已被拒绝。正常 v4 发送端不会触发。

---

## 🏗️ 代码结构

```
src/main.cpp          主程序（单文件实现）
  ├─ ProtocolHeader   协议头
  ├─ Frame I/O        sendFrame / recvFrame / writeFully / readFully
  ├─ BufferPool       流水线缓冲池
  ├─ SimpleCC         自定义拥塞控制（--no-cc）
  ├─ NetworkStats     统计快照与诊断
  ├─ Config           命令行解析
  └─ HruftPro         runSender / runReceiver
lib/                  blake3（四平台）+ json.hpp
CMakeLists.txt        构建配置
```

---

## ⚠️ 注意事项

1. **仅 IPv4**，不支持域名解析与 NAT 穿透
2. **明文传输**，无认证无加密。仅用于可信网络，不要暴露在公网
3. **单连接**：一次只处理一个对端，传完即退出
4. **`--no-cc` 慎用**：关闭拥塞控制后不会对丢包做退让，在共享链路上会挤占带宽甚至加剧拥塞。
   仅建议在直连光纤/局域网等无竞争环境使用
5. **内存占用**：`--window` 越大占用越多，两端各占约 `window × 2`（收发缓冲 + 内核 UDP 缓冲）

---

## 🔄 更新日志

### v4.0.0

**协议（破坏性）**
- 控制消息改为 `[u32 len][u8 type][payload]` 定长帧，修复「一次 recv 读到半条消息」导致的 ACK 误判、报告截断、文件名截断
- 协议头加版本号与 flags，magic 改为 `HRP4`
- 文件数据段仍为裸字节流，长度由 `file_size` 界定

**正确性修复**
- `recv <port> out.bin` 崩溃：`create_directories("")` 抛 `filesystem error`（空 parent 现在会跳过）
- 接收端在 `accept` 之后调用 `tuneSocket` 必然失败（UDT 此时已 `m_bOpened`），改为在 `bind` 前设置
- 放开 `UDT_FC`：此前接收窗口被 UDT 默认 25600 包截断，`--window` 超过约 37MB 完全无效
- 11 处 `setsockopt`、`listen`、`getsockopt` 返回值全部检查，失败即报错
- ACK 等待加 30s 超时，对端崩溃不再永久挂起
- 同名文件冲突改为循环加 `(1)(2)...`，不再依赖秒级时间戳（同秒会覆盖）
- 先写 `.part`，哈希通过才 `rename`；失败则删除
- 发送前后校验文件大小，避免传输中被修改导致接收端死等
- 端口与 `--mss` / `--window` / `--max-size` 全部做范围校验（端口 0、70000 此前会被 `htons` 静默截断）
- 发送目录时给出 `Not a regular file`，不再抛裸 `filesystem error`

**安全**
- 路径穿越防护：对端文件名只保留纯文件名，`../../evil.sh` → `evil.sh`，`/etc/passwd` → `passwd`
- 新增 `--max-size`，限制接收文件大小

**性能**
- 三段流水线：读盘 / BLAKE3 / 网络收发并行（`PIPELINE_DEPTH = 4` 块）
- 进度显示不再阻塞主传输路径

**工程**
- `CMakeLists.txt` 重写：UDT 自动探测、缺 `ccc.h` 明确报错、交叉编译真正生效（原配置在 `project()` 之后设编译器，被 CMake 静默忽略）
- 默认开启 `-Wall -Wextra`，当前零警告
- `GLOB` 加 `CONFIGURE_DEPENDS`，新增源文件不再需要手动重配
- 移除未使用的 `--detailed` 之外的所有死代码

### v3.0.0
- BLAKE3 替代 MD5
- 流式计算，超大文件不再二次读盘

### v2.0.0
- 流式传输优化

### v1.0.0
- 基础 UDT 文件传输

---

## 🔗 相关资源

- [UDT4（本项目使用）](https://github.com/JinBiLianShao/udt4)
- [BLAKE3](https://github.com/BLAKE3-team/BLAKE3)
- [nlohmann/json](https://json.nlohmann.me/)

## 📄 许可证

MIT
