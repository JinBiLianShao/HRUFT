# HRUFT Pro

基于 UDT4 的可靠 UDP 大文件传输工具。流式 BLAKE3 校验，传输结束输出 JSON 网络诊断报告。

作者：连思鑫

![C++](https://img.shields.io/badge/C%2B%2B-17-blue)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey)
![License](https://img.shields.io/badge/license-MIT-blue)
![Version](https://img.shields.io/badge/version-4.1.0-green)

---

## 为什么写这个

用普通 TCP 传大文件，跨运营商或者出国链路一丢包速度就塌到几百 KB/s，
等一个 100GB 的包要一整天。换成基于 UDP 的 UDT 之后同样的链路能跑满带宽，
于是有了这个工具。

它做的事很单一：把一台机器上的一个文件，尽可能快地、确定地送到另一台机器上，
并且能证明送过去的内容和源文件一个字节都不差。

---

## 特性

- **BLAKE3 流式校验**：边传边算，不二次读盘。传输结束后收发两端比对 256 位摘要
- **分帧协议**：控制消息带长度前缀，不依赖「一次 recv 恰好读到一条消息」这种不成立的假设
- **三段流水线**：读盘 / BLAKE3 / 网络收发并行
- **先落 `.part` 再改名**：哈希不过就删掉，磁盘上不会出现损坏文件
- **路径穿越防护**：对端文件名只保留纯文件名，`../../x` 一律降级
- **中文路径**：Windows 走 UTF-8 ↔ UTF-16 全程转换
- **参数全检查**：所有 `setsockopt` / `listen` / `recv` / `send` 返回值都看，失败即报错
- **不会无限挂起**：ACK 等待带 30 秒超时

---

## 快速开始

### 1. 准备 UDT4

```bash
git clone https://github.com/JinBiLianShao/udt4.git ../udt4
```

必须是**源码树**，不能是二进制安装。`--no-cc` 需要源码里的 `ccc.h`，
而它通常不在 `make install` 的产物里。CMake 会自动探测 `../udt4`，也可以显式指定：

```bash
cmake -S . -B build -DUDT_ROOT=/path/to/udt4
```

### 2. 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

CMake 会把 UDT4 当子项目一起编译。这样能避免一个坑：有些 UDT4 版本的
`CMakeLists.txt` 漏了 `ccc.cpp` 和 `cache.cpp`，用预编译库会链接失败
（`undefined reference to CCC::CCC()` / `CInfoBlock::getKey()`）。

### 3. 传文件

```bash
# 接收端先起
./build/hruft recv 9000 ./downloads/

# 发送端
./build/hruft send 192.168.1.100 9000 ./big.iso
```

---

## 参数

### 发送端

```bash
hruft send <ip> <port> <filepath> [options]
```

| 参数 | 说明 | 默认 |
|------|------|------|
| `ip` | 目标 IP（仅 IPv4，不支持域名/NAT 穿透） | 必填 |
| `port` | 目标端口，1-65535 | 必填 |
| `filepath` | 要发送的普通文件 | 必填 |
| `--mss` | 最大分段大小，536-8900 | 1500 |
| `--window` | 窗口大小（字节），最小 64KB，上限 256MB | 10MB |
| `--detailed` | 每秒输出瞬时速率 | 关 |
| `--no-cc` | 关闭拥塞控制（仅直连链路） | 关 |
| `--fast-start` | 跳过慢启动爬坡（小文件快 4 倍，弱网快 10~25 倍） | 关 |

### 接收端

```bash
hruft recv <port> <savepath> [options]
```

| 参数 | 说明 | 默认 |
|------|------|------|
| `port` | 监听端口，1-65535 | 必填 |
| `savepath` | 目录，或完整文件名 | 必填 |
| `--mss` | 同发送端 | 1500 |
| `--window` | 同发送端 | 10MB |
| `--max-size` | 拒绝大于该字节数的文件 | 不限 |
| `--detailed` | 每秒输出瞬时速率 | 关 |
| `--no-cc` | 关闭拥塞控制 | 关 |
| `--fast-start` | 跳过慢启动爬坡 | 关 |

---

## 调参：先说结论

### MSS 是影响最大的参数，而且必须两端一致

实测 512MB 传输：

| MSS | 包数 | 吞吐 |
|-----|------|------|
| 1500 | 368773 | 378 MB/s |
| 8900 | 60677 | **618 MB/s** |

包数降到 1/6，吞吐提升 63%。原因是 UDT 的每包开销是固定的，
吞吐基本被包数卡住。

> **必须两端给同一个值。** 我试过让发送端不设 MSS 靠 UDT 自己协商，
> 结果 UDT 取双方较小值，接收端 FC 按 8900 算包数、发送端却按 1500 发，
> 窗口严重失配，512MB 从 370 MB/s 掉到 **9.2 MB/s**。
> 功能测试第 15 条专门守这个，见 `tests/README.md`。

MSS 上限就是 8900，再大传输直接失败（实测 12000 / 16000 / 32768 / 65535 全部不行）。

### 窗口：接收端设才有用

UDT 的接收窗口由**接收端**决定，发送端只是把对方通告的窗口当上限。
所以 `send --window 128MB` 在接收端还是默认 10MB 时**毫无效果**。

另外窗口不是越大越好。要让 `--window` 真正生效，程序内部必须同时设 `UDT_FC`
（飞行窗口，单位是**包**）——UDT 默认 FC 只有 25600 包，MSS 1500 时相当于
37MB 硬上限，`--window` 给多大都会被截断到这儿。本程序已经处理了。

实测流水线深度 4 / 8 / 16 对吞吐没有可见影响（564 / 569 / 561 MB/s），
所以固定 4 块，不再加深。

### `--no-cc` 能提多少

直连链路上实测从 125 提到 236 MB/s（128MB 文件）。因为不存在中间链路拥塞，
UDT 默认的慢启动和丢包退让纯属拖后腿。

> 但共享链路上不要用。关掉拥塞控制意味着丢包时不会退让，
> 会挤占别人的带宽，也可能让整条链路更堵。

### `--fast-start`：弱网和小文件的救命选项

UDT 默认拥塞控制从 16 个包的窗口开始爬坡，每次翻倍要等 10ms。传大文件无所谓，
但小文件几乎全耗在爬坡上。实测：

| 场景 | 默认 | `--fast-start` |
|------|------|-----------------|
| 16MB，无丢包 | 25 MB/s | **116 MB/s** |
| 64MB，无丢包 | 97 MB/s | **411 MB/s** |
| 64MB，丢包 1% | 11 MB/s | **130 MB/s** |
| 64MB，丢包 2% | 6.8 MB/s | **128 MB/s** |
| 64MB，丢包 5% | 2.4 MB/s | **60 MB/s** |

**丢包越严重，差距越大**。因为丢包让 ACK 变稀疏，默认模式的拥塞窗口
会卡在 16 个包上不去（报告里 `window_cong: 16` 而 `window_flow: 15128`）。

> 同样不适合公网共享链路——它不做拥塞退让。适合的是**链路差但可控**的场景，
> 比如自家专线、卫星链路，默认模式在那里会退化到几 MB/s 完全没法用。

---

## 协议（v4）

> v4 与 v3 **不互通**，收发两端必须一起升级。

### 帧格式

```
+-----------------+----------+--------------------+
| u32 长度 (大端)  | u8 类型  | payload (长度-1)    |
+-----------------+----------+--------------------+
```

长度字段把类型字节也算进去，上限 64MB。

| 类型 | 名称 | payload |
|------|------|---------|
| 1 | HEADER | `ProtocolHeader`(24B) + 文件名(UTF-8) |
| 2 | HASH | 32 字节 BLAKE3 |
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

### 时序

```
发送端                                    接收端
  |---- HEADER(头+文件名) --------------->|
  |---- 文件数据（裸字节流，无帧）------->|
  |---- HASH(32B) ---------------------->|
  |---- DONE --------------------------->|
  |<--- ACK(1B) ------------------------|
  |<--- REPORT(JSON) -------------------|
```

文件数据不加帧，长度由 `file_size` 精确界定——给每 4MB 套一层帧头纯属浪费。

---

## CMake 选项

| 选项 | 说明 | 默认 |
|------|------|------|
| `CMAKE_BUILD_TYPE` | `Release` / `Debug` | Release |
| `UDT_ROOT` | UDT4 源码树或安装前缀 | 自动探测 `../udt4` |
| `TARGET_OS` | `linux` / `windows` | linux |
| `COMPILER_TARGET` | `x86` / `arm32` / `arm64` | x86 |
| `ENABLE_OPENMP` | 启用 OpenMP | OFF |

```bash
# 交叉编译 arm64
cmake -S . -B build-arm64 -DCOMPILER_TARGET=arm64 -DCMAKE_BUILD_TYPE=Release
cmake --build build-arm64 -j
```

---

## 输出报告

接收端传完会生成 JSON，本地打印并发回发送端：

```json
{
  "meta": {
    "filename": "big.iso",
    "filepath": "/data/big.iso",
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

| 等级 | 条件 |
|------|------|
| `excellent` | 重传率 < 0.1%，且无窗口告警 |
| `good` | 重传率 0.1% - 1% |
| `suboptimal` | 配置窗口小于 BDP |
| `network_lossy` | 重传率 > 1% |
| `unknown` | 统计数据缺失 |

---

## 测试

```bash
# 全部测试（约 2 分钟）
./tests/run_all.sh

# 性能基准
HRUFT_BIN=./build/hruft ./tests/performance/bench.sh
```

目前 80 个用例：功能 23、安全 23、健壮性 27、丢包环境 7。
测试说明和实测数据在 [`tests/README.md`](tests/README.md)。

---

## 故障排除

### 编译期

**`UDT4 headers not found`**
没克隆 UDT4 或路径不对。`git clone https://github.com/JinBiLianShao/udt4.git ../udt4`。

**`missing ccc.h`**
你指向的是二进制安装目录。`--no-cc` 需要源码树里的 `ccc.h`。

**`undefined reference to CCC::CCC()` / `CInfoBlock::getKey()`**
UDT4 的 `CMakeLists.txt` 漏了 `src/ccc.cpp` 和 `src/cache.cpp`。补进 `UDT_SOURCES`。

### 运行期

**速度远低于预期**
1. 看报告里 `congestion.window_flow`。如果停在 25600 附近，说明 `UDT_FC` 没放开
2. 确认 `--window` 是在**接收端**调的
3. 试试 `--mss 8900`，**两端都加**
4. 直连链路可以试 `--no-cc`

**哈希校验失败**
接收端会删掉 `.part`，返回非 0。检查链路稳定性后重传。

**接收端报 `Unsafe filename`**
对端文件名含路径分隔符，已被拒绝。正常 v4 发送端不会触发。

---

## 已知限制

- **仅 IPv4**，不支持域名解析和 NAT 穿透
- **明文传输**，无认证无加密，仅用于可信网络
- **单连接**：一次处理一个对端，传完退出
- **`--no-cc` 慎用**：共享链路上会挤占带宽
- **内存占用**约 `window × 2`（收发缓冲 + 内核 UDP 缓冲）
- **Windows 分支未经实机验证**：代码写了，但开发机没有 Windows 环境

---

## 更新日志

### v4.1.0

**性能**
- 新增 `--fast-start`：跳过 UDT 慢启动爬坡。16MB 文件快 4.6 倍，
  2% 丢包环境下快 18.8 倍（实测数据见 `tests/README.md`）
- 定位到默认模式在丢包环境下的真实瓶颈：`CUDTCC` 的拥塞窗口
  卡在 16 个包不增长（`window_cong: 16` vs `window_flow: 15128`）

**报告修正**
- 修复可靠性统计口径错误：`retrans_ratio` 在接收端恒为 0，
  导致「网络优秀」的错误结论。新增 `recv_loss_ratio`，
  实测 5% 丢包下能正确报出 4.32%
- 网络诊断改用真实丢包率，不再被恒零字段误导

**测试**
- 新增丢包环境测试套件（unshare + tc netem），7 个用例
- 修复 ARM 交叉编译：编译器改在 project() 之前设定（CMake 官方要求），
  一次配置即可用，不再需要两段式重跑；实测 aarch64 产物构建成功
- 修复 --no-cc 与 --fast-start 同时指定时被静默忽略的问题，现在直接报错
- 修复 tests/run_all.sh 的 --full 说明与实际行为不一致：
  512MB 大文件用例改为仅在 --full（或 HRUFT_TEST_BIG=1）时运行
- 用 netem 验证了 `--fast-start` / `--no-cc` 在 1%/2%/5% 丢包下的
  正确性与稳定性
- 量化了 loopback 噪声水平（±13%），此后的性能结论都以此为门槛

### v4.0.0

**协议（破坏性）**
- 控制消息改为 `[u32 len][u8 type][payload]` 定长帧
- 修复「一次 recv 读到半条消息」导致的 ACK 误判、报告截断、文件名截断
- 协议头加版本号与 flags，magic 改为 `HRP4`

**正确性**
- 修复 `recv <port> out.bin` 崩溃（`create_directories("")` 抛异常）
- 修复接收端在 `accept` 后调 `tuneSocket` 必然失败（UDT 已 `m_bOpened`）
- 放开 `UDT_FC`：此前接收窗口被默认 25600 包截断，`--window` 超 37MB 无效
- 所有 `setsockopt` / `listen` / `getsockopt` 返回值检查
- ACK 等待加 30s 超时
- 同名冲突改循环编号，不再依赖秒级时间戳
- 先写 `.part` 校验通过再 `rename`
- 发送前后校验文件大小，防止传输中被修改
- 端口 / MSS / 窗口 / max-size 全部范围校验
- 发送目录给出明确错误，不再抛裸 `filesystem error`

**安全**
- 路径穿越防护：对端文件名只保留纯文件名
- 文件名 NUL 字节与非法 UTF-8 检查
- 新增 `--max-size`

**性能**
- 三段流水线：读盘 / BLAKE3 / 网络收发并行
- `UDT_FC` 与 `UDT_RCVBUF` 配合设置，大窗口真正生效
- 进度显示不阻塞主传输路径

**工程**
- 新增测试套件：功能 23 + 安全 23 + 健壮性 27，共 73 个用例
- `CMakeLists.txt` 重写：UDT 自动探测、缺 `ccc.h` 明确报错、交叉编译真正生效
- 默认 `-Wall -Wextra`，当前零警告
- 源码注释重写，记录每个决策的原因和踩过的坑

### v3.0.0
- BLAKE3 替代 MD5，流式计算

### v2.0.0
- 流式传输优化

### v1.0.0
- 基础 UDT 文件传输

---

## 许可证

MIT

## 相关

- [UDT4（本项目使用）](https://github.com/JinBiLianShao/udt4)
- [BLAKE3](https://github.com/BLAKE3-team/BLAKE3)
- [nlohmann/json](https://json.nlohmann.me/)
