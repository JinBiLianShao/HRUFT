# HRUFT - 高性能可靠UDP文件传输系统

## 概述

HRUFT是一个基于UDP的高性能可靠文件传输系统，旨在为高延迟网络环境和大文件传输提供优化的解决方案。系统采用C++17开发，支持跨平台（Linux/Windows）部署，提供完整的数据可靠性保证和传输统计功能。

## 核心特性

- **基于UDP的高性能传输**：在UDP基础上实现可靠传输，避免TCP在高延迟环境下的性能瓶颈
- **自适应传输参数**：支持动态调整MSS（最大分段大小）和窗口大小以适应不同网络条件
- **端到端数据完整性校验**：使用MD5校验确保文件传输的完整性
- **详细的传输统计**：实时监控传输速度、丢包率、重传率等关键指标
- **跨平台支持**：支持Linux和Windows平台，提供统一的编译和使用体验
- **JSON格式进度报告**：便于与其他系统集成和自动化监控

## 系统架构

### 传输模型
HRUFT采用两级传输模型：
1. **控制平面**：负责握手协商、传输参数同步
2. **数据平面**：负责实际文件数据的传输

### 可靠性机制
- 基于UDT（UDP-based Data Transfer）协议的可靠传输
- 支持数据包重传和流量控制
- 提供完整的丢包检测和恢复机制

## 快速开始

### 构建项目

```bash
# 创建构建目录
mkdir build && cd build

# 配置CMake（Linux示例）
cmake .. -DTARGET_OS=linux -DCOMPILER_TARGET=x86 -DTARGET_TYPE=executable

# 编译
make -j4
```

### 使用示例

**发送文件：**
```bash
./hruft send <目标IP> <端口> <文件路径> [选项]
示例：./hruft send 192.168.1.100 9000 /path/to/large_file.iso --mss 1500 --window 1048576
```

**接收文件：**
```bash
./hruft recv <监听端口> <保存路径> [选项]
示例：./hruft recv 9000 /path/to/save/received_file.iso
```

## 命令行参数

### 发送端参数
| 参数 | 描述 | 默认值 | 必填 |
|------|------|--------|------|
| mode | 传输模式（send/recv） | - | 是 |
| ip | 目标服务器IP地址 | - | 是 |
| port | 目标端口 | - | 是 |
| filepath | 要发送的文件路径 | - | 是 |
| --mss | 最大分段大小（字节） | 1500 | 否 |
| --window | 传输窗口大小（字节） | 1048576 | 否 |
| --detailed | 启用详细统计 | false | 否 |

### 接收端参数
| 参数 | 描述 | 默认值 | 必填 |
|------|------|--------|------|
| mode | 传输模式（recv） | - | 是 |
| port | 监听端口 | - | 是 |
| savepath | 文件保存路径 | - | 是 |
| --detailed | 启用详细统计 | false | 否 |

## 传输统计

HRUFT提供全面的传输统计信息，包括：

### 实时监控指标
- 传输进度百分比
- 当前传输速度（Mbps）
- 平均传输速度（Mbps）
- 最大/最小传输速度
- 已传输字节数/总字节数
- 预计剩余时间

### 网络质量指标
- 总发送/接收包数
- 发送端/接收端丢包数
- 丢包率（%）
- 重传包数
- 重传率（%）
- 传输效率（有效包/总包数）

### 输出格式
统计信息以JSON格式输出，便于解析和集成：
```json
{
  "type": "statistics",
  "total_bytes": 1073741824,
  "transferred_bytes": 536870912,
  "completion_percentage": 50.00,
  "average_speed_mbps": 85.24,
  "max_speed_mbps": 120.50,
  "min_speed_mbps": 45.30,
  "packet_stats": {
    "pktSentTotal": 10000,
    "pktRecvTotal": 9980,
    "pktSndLossTotal": 20,
    "loss_rate": 0.20
  },
  "efficiency_level": "excellent",
  "suggestions": "Network conditions are good."
}
```

## 构建选项

CMake构建系统支持多种配置选项：

| 选项 | 描述 | 可选值 | 默认值 |
|------|------|--------|--------|
| TARGET_OS | 目标操作系统 | linux, windows | linux |
| COMPILER_TARGET | 目标架构 | x86, arm32, arm64 | x86 |
| TARGET_TYPE | 目标类型 | executable, static, shared | executable |
| ENABLE_OPENMP | 启用OpenMP并行 | ON, OFF | OFF |
| PUBLIC_PACKAGE_DIR | 外部包目录路径 | 路径或"NO" | NO |

### 交叉编译示例
```bash
# ARM64目标
cmake .. -DCOMPILER_TARGET=arm64 -DTARGET_TYPE=executable

# Windows目标（使用MinGW）
cmake .. -DTARGET_OS=windows -DTARGET_TYPE=executable

# 静态库构建
cmake .. -DTARGET_TYPE=static
```

## 协议设计

### 握手协议
传输开始前，发送端和接收端通过握手协议协商传输参数：
- 文件大小
- MD5校验值
- MSS（最大分段大小）
- 窗口大小

### 数据格式
所有协议结构体使用紧凑内存布局（`#pragma pack(push, 1)`）确保跨平台兼容性。

## 开发指南

### 项目结构
```
hruft/
├── CMakeLists.txt          # 构建配置文件
├── src/                    # 源代码目录
├── include/                # 头文件目录
├── lib/                    # 第三方库目录
├── external/               # 外部依赖目录
└── build/                  # 构建目录
```

### 依赖项
- UDT库（UDP-based Data Transfer）
- 标准C++17库
- 跨平台网络库（Winsock2 / BSD sockets）

### 扩展开发
如需扩展功能，可修改以下部分：
- 在`protocol/`目录中添加新的协议定义
- 在`net/`目录中扩展网络功能
- 在`security/`目录中增强安全功能

## 性能优化建议

1. **调整窗口大小**：根据网络延迟调整传输窗口大小，高延迟网络建议增大窗口
2. **优化MSS**：根据MTU（最大传输单元）调整MSS，避免IP分片
3. **并行传输**：对于特大文件，可考虑分块并行传输
4. **内存管理**：使用内存映射文件（mmap）减少内存拷贝开销

## 故障排除

### 常见问题

1. **连接失败**
    - 检查防火墙设置
    - 确认目标端口是否开放
    - 验证IP地址和端口号

2. **传输速度慢**
    - 检查网络带宽限制
    - 调整传输窗口大小
    - 检查是否有网络丢包

3. **校验失败**
    - 确保发送和接收的文件路径正确
    - 检查磁盘空间是否充足
    - 验证网络传输过程中是否有数据损坏

### 调试信息
启用详细统计模式可获取更多调试信息：
```bash
./hruft send 192.168.1.100 9000 file.iso --detailed
```

## 许可证

本项目采用MIT许可证。详见LICENSE文件。

## 贡献指南

欢迎提交问题报告和功能请求。提交代码前请确保：
1. 代码符合项目编码规范
2. 添加必要的测试用例
3. 更新相关文档

## 版本历史

- v1.0.0 (初始版本)
    - 基于UDP的可靠文件传输
    - 支持跨平台编译和运行
    - 完整的传输统计功能
    - 数据完整性校验

## 技术支持

如有技术问题，请：
1. 查看项目文档
2. 检查已存在的问题报告
3. 提交新的Issue

---

**注意**：本系统专为可控网络环境设计，不适用于需要NAT穿透或公网P2P传输的场景。