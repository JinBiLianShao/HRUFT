# 高性能可靠 UDP 文件传输系统

**HRUFT（High-performance Reliable UDP File Transfer）**
**完整设计报告（工程版）**

---

## 0. 设计目标与约束

### 0.1 设计目标

HRUFT 是一个 **基于 UDP 的高性能、强可靠、大文件传输工具**，目标：

* 在 **高时延（High RTT）网络环境** 下仍能接近链路带宽上限
* 支持 **超大文件（≥ TB 级）**
* 提供 **强一致性校验 + 安全防护**
* **跨平台（Linux / Windows）**
* **纯命令行工具**
* 使用 **C++17**
* **不依赖重量级第三方库**

---

### 0.2 明确不做的事情（边界）

* ❌ 不做 NAT 穿透
* ❌ 不做公网 P2P
* ❌ 不追求 TCP 兼容
* ❌ 不依赖系统 TCP 拥塞控制

---

## 1. 总体架构设计

### 1.1 架构总览

```
+------------------------------------------------+
|                    HRUFT                       |
+------------------------------------------------+
| CLI Interface                                  |
+------------------------------------------------+
| Session Manager                                |
+------------------------------------------------+
| Control Plane (UDP : 10000)                    |
|  - handshake                                   |
|  - chunk scheduling                            |
|  - ACK / NACK                                  |
+------------------------------------------------+
| Data Plane (UDP : 10001 ~ 10001+N)             |
|  - parallel chunk transfer                     |
|  - packet-level reliability                    |
+------------------------------------------------+
| Memory Manager                                 |
|  - mmap / MapViewOfFile                        |
+------------------------------------------------+
| Security Layer                                 |
|  - AES-CTR                                     |
|  - HMAC-SHA256                                 |
+------------------------------------------------+
| Platform Abstraction                           |
|  - socket / file / time                        |
+------------------------------------------------+
```

---

### 1.2 核心思想总结

| 维度    | 设计选择                 |
| ----- | -------------------- |
| 传输模型  | Chunk + Packet 两级    |
| 并行度   | Chunk 级并行            |
| 可靠性   | 应用层 ARQ              |
| 高时延优化 | Chunk Sliding Window |
| 大文件   | mmap 零拷贝             |
| 安全    | AEAD 风格              |

---

## 2. 命令行接口（CLI）

### 2.1 应用名

```bash
hruft
```

---

### 2.2 参数定义

| 参数     | 说明             | 示例                |
| ------ | -------------- | ----------------- |
| `-m`   | 模式：send / recv | `-m send`         |
| `-f`   | 文件路径           | `-f ./big.iso`    |
| `-i`   | 目标 IP（send）    | `-i 192.168.1.10` |
| `-t`   | 工作线程数          | `-t 8`            |
| `-c`   | Chunk 大小（MB）   | `-c 4`            |
| `-w`   | Chunk 窗口大小     | `-w 16`           |
| `-p`   | 起始数据端口         | `-p 10001`        |
| `--key` | 加密密钥           | `--key secret`    |

---

## 3. 协议设计（Protocol Specification）

### 3.1 基础约定

* **小端序**
* 所有结构体使用：

```cpp
#pragma pack(push, 1)
#pragma pack(pop)
```

---

## 3.2 控制平面协议（Port 10000）

### 3.2.1 控制类型

```cpp
enum class ControlType : uint8_t {
    SYN,
    SYN_ACK,
    CHUNK_META,
    CHUNK_CONFIRM,
    CHUNK_RETRY,
    FILE_DONE
};
```

---

### 3.2.2 ControlPacket 布局

```cpp
struct ControlPacket {
    uint32_t magic;      // 'HRUF'
    uint16_t version;    // 0x0001
    ControlType type;
    uint32_t chunkId;
    uint16_t payloadLen;
    uint8_t  payload[1024];
};
```

---

### 3.2.3 控制流语义

| 类型            | 作用                 |
| ------------- | ------------------ |
| SYN           | 文件名 / 大小 / chunk 数 |
| SYN_ACK       | 磁盘空间确认             |
| CHUNK_META    | chunk hash         |
| CHUNK_CONFIRM | chunk 校验成功         |
| CHUNK_RETRY   | chunk 丢包位图         |
| FILE_DONE     | 整体完成               |

---

## 3.3 数据平面协议（Port 10001+）

### 3.3.1 DataPacket 布局（核心）

```cpp
struct DataPacket {
    uint32_t magic;
    uint16_t version;
    uint32_t chunkId;
    uint32_t seq;
    uint64_t offset;     // 文件绝对偏移
    uint16_t dataLen;
    uint16_t flags;
    uint32_t crc32;
    uint8_t  data[];
};
```

---

### 3.3.2 flags 定义

| Flag | 含义          |
| ---- | ----------- |
| 0x01 | LAST_PACKET |
| 0x02 | RETRANSMIT  |

---

## 4. 高时延优化设计（核心价值）

### 4.1 Chunk Sliding Window

* 同时在飞多个 Chunk
* Chunk 完成顺序 ≠ Chunk 发送顺序
* 控制线程统一调度

```
Window = [C5 C6 C7 C8 C9 C10 ...]
```

---

### 4.2 与 TCP 的本质差异

| TCP           | HRUFT        |
| ------------- | ------------ |
| packet window | chunk window |
| 顺序强约束         | 完全乱序         |
| 单流            | 多流           |
| 内核控制          | 应用控制         |

---

### 4.3 高 RTT 数学模型（直观）

```
Throughput ≈ WindowSize × ChunkSize / RTT
```

HRUFT 可 **人为放大 WindowSize**
TCP 不能

---

## 5. 内存管理设计（mmap）

### 5.1 发送端

* mmap 整个文件
* Chunk = 指针偏移
* 无中间缓冲

---

### 5.2 接收端

1. `resize_file`
2. mmap 整个文件
3. 根据 offset 写入

```cpp
memcpy(mapped_base + offset, data, len);
```

---

### 5.3 mmap 注意事项

| 平台      | 关键点       |
| ------- | --------- |
| Linux   | 可延迟 msync |
| Windows | 偏移对齐      |

---

## 6. 可靠性机制

### 6.1 分层设计

| 层级     | 机制      |
| ------ | ------- |
| Packet | CRC32   |
| Chunk  | SHA-256 |
| File   | SHA-256 |

---

### 6.2 丢包恢复

* Packet 丢失 → bitmap
* Chunk NACK → 只补丢失包
* 不整块重传

---

## 7. 安全设计

### 7.1 加密模型

```
Data → AES-CTR → Cipher
Cipher + Header → HMAC → Tag
```

---

### 7.2 安全属性

| 威胁 | 防护          |
| -- | ----------- |
| 篡改 | HMAC        |
| 伪造 | HMAC        |
| 重放 | seq + nonce |
| 窃听 | AES         |

---

## 8. 跨平台抽象层

### 8.1 Socket 抽象

```cpp
class UdpSocket {
public:
    bool bind(uint16_t port);
    ssize_t sendTo(...);
    ssize_t recvFrom(...);
};
```

---

### 8.2 平台差异封装

| 项     | Linux | Windows       |
| ----- | ----- | ------------- |
| init  | 无     | WSAStartup    |
| close | close | closesocket   |
| mmap  | mmap  | MapViewOfFile |

---

## 9. 线程模型

### 9.1 发送端

* 1 × Control Thread
* N × Worker Threads

---

### 9.2 接收端

* 1 × Control Thread
* N × Data Receiver Threads
* 1 × Integrity Thread

---

## 10. 状态机设计

```
INIT
 ↓
HANDSHAKE
 ↓
TRANSFER
 ↓
VERIFY
 ↓
DONE
```

---

## 11. 监控与日志

```
[45.2%] Speed=118MB/s RTT=180ms
Chunks: 128/512
Retries: 3
```

---

## 12. 项目目录结构建议

```
hruft/
 ├─ protocol/
 ├─ net/
 ├─ core/
 ├─ security/
 ├─ platform/
 ├─ cli/
 └─ main.cpp
```

---

## 13. 结论

> **HRUFT 是一个为“高时延 + 大文件 + 可控性能”而生的 UDP 可靠传输系统。**

它不是 TCP 的替代，而是 **TCP 在极端场景下的工程级超集方案**。

