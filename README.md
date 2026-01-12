# HRUFT - 高性能可靠UDP文件传输系统

![Build Status](https://img.shields.io/badge/build-passing-brightgreen)
![License](https://img.shields.io/badge/license-MIT-blue)
![C++ Version](https://img.shields.io/badge/C++-17-blue)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey)

HRUFT（High-performance Reliable UDP File Transfer）是一个基于UDT协议的高性能可靠文件传输系统，专为高延迟网络和大文件传输场景设计。

## ✨ 主要特性

- **高性能传输**：基于UDP的可靠传输，避免TCP在高延迟环境下的性能瓶颈
- **自适应配置**：支持动态调整MSS和窗口大小以适应不同网络条件
- **端到端校验**：使用MD5校验确保文件传输的完整性
- **详细统计**：提供全面的传输统计和网络分析指标
- **跨平台支持**：支持Windows和Linux平台
- **JSON格式输出**：便于系统集成和自动化监控
- **智能网络评估**：自动评估网络质量并提供优化建议

## 📦 系统要求

### 操作系统
- Windows 7/8/10/11
- Linux (Ubuntu, CentOS, Debian等)

### 编译器
- GCC 7.0+ (Linux)
- MinGW-w64 或 Visual Studio 2017+ (Windows)

### 依赖库
- UDT库（需要自行导入）
- 标准C++17库

## 🚀 快速开始

### 1. 克隆项目
```bash
git clone https://github.com/JinBiLianShao/HRUFT.git
cd hruft
```

### 2. 构建项目
```bash
mkdir build
cd build
cmake .. （根据系统要求选择）
make -j4
```

### 3. 发送文件
```bash
# Linux
./hruft send <目标IP> <端口> <文件路径> [选项]

# Windows
hruft.exe send <目标IP> <端口> <文件路径> [选项]
```

### 4. 接收文件
```bash
# Linux
./hruft recv <监听端口> <保存路径> [选项]

# Windows
hruft.exe recv <监听端口> <保存路径> [选项]
```

## 📖 详细使用指南

### 发送端命令
```bash
hruft send <ip> <port> <filepath> [选项]
```

**参数说明：**

| 参数 | 描述 | 默认值 | 必填 |
|------|------|--------|------|
| `ip` | 目标服务器IP地址 | - | 是 |
| `port` | 目标端口 | - | 是 |
| `filepath` | 要发送的文件路径 | - | 是 |
| `--mss` | 最大分段大小（字节） | 1500 | 否 |
| `--window` | 传输窗口大小（字节） | 1048576 | 否 |
| `--detailed` | 启用详细统计输出 | false | 否 |


**示例：**
```bash
hruft send 192.168.1.100 9000 ./large_file.iso --mss 1024 --window 104857600 --detailed
```

### 接收端命令
```bash
hruft recv <port> <savepath> [选项]
```

**参数说明：**

| 参数 | 描述 | 默认值 | 必填 |
|------|------|--------|------|
| `port` | 监听端口 | - | 是 |
| `savepath` | 文件保存路径 | - | 是 |
| `--detailed` | 启用详细统计输出 | false | 否 |

**示例：**
```bash
hruft recv 9000 ./received_file.iso --detailed
```

## 🔧 高级配置

### CMake构建选项

| 选项 | 描述         | 可选值                        | 默认值 |
|------|------------|----------------------------|--------|
| `DTARGET_OS` | 目标操作系统     | linux, windows             | linux |
| `DCOMPILER_TARGET` | 目标架构       | x86, arm32, arm64          | x86 |
| `DTARGET_TYPE` | 目标类型       | executable, static, shared | executable |
| `DENABLE_OPENMP` | 启用OpenMP并行 | ON, OFF                    | OFF |
| `DPUBLIC_PACKAGE_DIR` | 外部包/库目录路径  | 路径或"NO"                    | NO |
| `DCMAKE_BUILD_TYPE` | 构建类型     | Release或Debug              | Debug |

**构建示例：**
```bash
# ARM64架构
cmake .. -DCOMPILER_TARGET=arm64 -DTARGET_TYPE=executable

# Windows目标（使用MinGW）
cmake .. -DTARGET_OS=windows -DCOMPILER_TARGET=x86 -DPUBLIC_PACKAGE_DIR=/home/lsx/Code/udt4/src -DTARGET_TYPE=executable -DCMAKE_BUILD_TYPE=Release

# 静态库构建
cmake .. -DTARGET_TYPE=static
```

### 传输参数调优

1. **MSS（最大分段大小）**
    - 建议值：1024-1500字节
    - 应根据网络MTU调整，避免IP分片

2. **窗口大小**
    - 默认：1MB（1048576字节）
    - 高延迟网络建议增大窗口
    - 公式：窗口大小 ≈ 带宽 × RTT

3. **流控制**
    - 系统自动计算流控制窗口
    - 最小值为25600个包

## 📊 统计信息说明

HRUFT提供全面的传输统计信息，帮助用户评估网络状况和传输效率。

### 实时进度报告
```json
{
  "type": "progress",
  "percent": 50,
  "current": 536870912,
  "total": 1073741824,
  "speed_mbps": 256.42,
  "elapsed_seconds": 2,
  "average_speed_mbps": 250.18,
  "remaining_bytes": 536870912
}
```

### 完整统计报告
HRUFT提供两种统计报告：

1. **JSON格式**：便于程序解析和集成
2. **文本格式**：便于人类阅读

#### 关键统计指标

**速度统计：**
- 平均速度：整个传输过程的平均速度
- 最高速度：传输过程中达到的最高瞬时速度
- 最低速度：传输过程中的最低瞬时速度
- 当前速度：最后一次测量的瞬时速度

**UDT原始包统计：**
- 总发送包数：包括数据包和控制包
- 总接收包数：包括数据包和控制包
- 数据包丢失数：发送端和接收端分别统计
- 重传包数：需要重传的数据包数
- 控制包统计：ACK和NAK包的数量

**网络分析指标：**
- 数据包丢包率：丢失的数据包比例
- 控制包开销：控制包占总体流量的比例
- 网络传输效率：有效数据传输比例
- 数据完整性评分：基于丢包率和重传率计算的评分

### 网络质量评估
系统会根据统计数据自动评估网络质量：

| 质量等级 | 丢包率 | 说明 |
|----------|--------|------|
| Excellent | < 2% | 网络状况极佳 |
| Good | 2%-5% | 网络状况良好 |
| Fair | 5%-10% | 网络状况一般 |
| Poor | > 10% | 网络状况较差 |

## 🏗️ 项目结构

```
hruft/
├── CMakeLists.txt          # CMake构建配置文件
├── README.md              # 项目说明文档
├── main.cpp              # 主程序文件
├── udt4/                  # UDT4网络库
└── build/                # 构建目录
```

### 核心模块

1. **传输统计模块** (`TransferStatistics`类)
    - 实时速度计算和统计
    - UDT性能数据收集
    - 网络质量评估

2. **网络传输模块**
    - UDT socket管理
    - 自适应参数调整
    - 可靠数据传输

3. **文件处理模块**
    - 大文件分块传输
    - MD5校验计算
    - 进度报告生成

## 🔍 故障排除

### 常见问题

#### 1. 连接失败
- 检查目标IP和端口是否正确
- 确认防火墙设置
- 验证网络连通性

#### 2. 传输速度慢
- 调整窗口大小参数
- 检查网络带宽限制
- 考虑使用更大的MSS值

#### 3. 统计信息异常
- 确认使用了`--detailed`参数
- 检查网络稳定性
- 验证文件完整性

#### 4. 编译错误
- 确认已安装所有依赖
- 检查编译器版本
- 验证CMake配置

### 调试模式
启用详细统计可以获取更多调试信息：
```bash
hruft send 192.168.1.100 9000 file.iso --detailed
```


## 🤝 贡献指南

### 提交问题
1. 在GitHub Issues中搜索相关问题
2. 如未找到，创建新Issue并详细描述问题
3. 包括操作系统、编译器和复现步骤

### 提交代码
1. Fork项目到个人账户
2. 创建功能分支
3. 提交清晰的提交信息
4. 创建Pull Request

### 代码规范
- 遵循C++17标准
- 使用有意义的变量名
- 添加必要的注释
- 保持代码风格一致

## 📄 许可证

本项目采用MIT许可证。详见[LICENSE](LICENSE)文件。

## 🙏 致谢

- **UDT项目**：提供高性能UDP传输协议
- **CMake社区**：跨平台构建系统
- **所有贡献者**：感谢每一位为项目做出贡献的人

## 📞 联系方式

- **项目维护者**：连思鑫
- **问题反馈**：[GitHub Issues](https://github.com/JinBiLianShao/HRUFT/issues)
- **邮箱**：2013182991@qq.com  |  jinbilianshao@gmail.com

---

## ⚡ 快速参考表

| 命令 | 说明 |
|------|------|
| `hruft send <ip> <port> <file> --detailed` | 发送文件并显示详细统计 |
| `hruft recv <port> <save> --detailed` | 接收文件并显示详细统计 |
| `--mss 1024` | 设置最大分段大小为1024字节 |
| `--window 104857600` | 设置传输窗口为100MB |

| 统计项 | 理想值 | 说明 |
|--------|--------|------|
| 丢包率 | < 2% | 网络状况良好 |
| 控制开销 | < 10% | 控制包开销合理 |
| 传输效率 | > 95% | 数据传输高效 |
| 完整性评分 | > 90 | 数据完整性好 |

---

**提示**：对于高延迟网络（>100ms RTT），建议增大窗口大小以获得更好的性能。

**注意**：本系统专为可控网络环境设计，不适用于需要NAT穿透或公网P2P传输的场景。

---

⭐ **如果这个项目对你有帮助，请给它一个Star！** ⭐