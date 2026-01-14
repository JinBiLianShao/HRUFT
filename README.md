# HRUFT - 高性能可靠UDP文件传输系统 (最终版本)

![Build Status](https://img.shields.io/badge/build-passing-brightgreen)
![License](https://img.shields.io/badge/license-MIT-blue)
![C++ Version](https://img.shields.io/badge/C++-17-blue)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey)

HRUFT（High-performance Reliable UDP File Transfer）是一个基于UDT协议的高性能可靠文件传输系统，专为高延迟网络和大文件传输场景设计。

## ✨ 主要特性

- **高性能传输**：基于UDP的可靠传输，避免TCP在高延迟环境下的性能瓶颈
- **智能同步**：发送端与接收端完全同步，确保所有数据处理完成
- **自适应配置**：支持动态调整MSS和窗口大小以适应不同网络条件
- **端到端校验**：使用MD5校验确保文件传输的完整性
- **详细统计**：提供全面的传输统计和网络分析指标
- **跨平台支持**：支持Windows和Linux平台
- **JSON格式输出**：便于系统集成和自动化监控
- **智能网络评估**：自动评估网络质量并提供优化建议
- **可靠的结束信令**：完整的传输确认机制，确保双方同步关闭

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
- 文件系统库（C++17）

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
# 发送大文件并使用优化参数
hruft send 192.168.1.100 9000 ./large_file.iso --mss 1024 --window 104857600 --detailed

# 简单发送
hruft send 192.168.1.100 9000 ./document.pdf
```

### 接收端命令
```bash
hruft recv <port> <savepath> [选项]
```

**参数说明：**

| 参数 | 描述 | 默认值 | 必填 |
|------|------|--------|------|
| `port` | 监听端口 | - | 是 |
| `savepath` | 文件保存路径或目录 | - | 是 |
| `--detailed` | 启用详细统计输出 | false | 否 |

**示例：**
```bash
# 接收到指定目录（自动使用发送端文件名）
hruft recv 9000 ./downloads/ --detailed

# 接收到指定文件路径
hruft recv 9000 ./downloads/my_file.zip
```

## 🔄 传输流程说明

HRUFT采用完整的握手和确认机制确保可靠传输：

### 发送端流程：
1. **连接建立**：连接到接收端，发送握手包
2. **数据传输**：分块发送文件数据
3. **结束标记**：发送"FILE_TRANSMISSION_COMPLETE_EOF"标记
4. **等待确认**：等待接收端完成接收并返回MD5校验结果
5. **发送统计**：发送详细的传输统计信息
6. **优雅关闭**：等待接收端确认后关闭连接

### 接收端流程：
1. **监听连接**：监听指定端口等待发送端连接
2. **接收握手**：接收握手包，获取文件信息和配置
3. **接收数据**：持续接收数据直到收到结束标记
4. **MD5校验**：计算接收文件的MD5值
5. **发送结果**：将MD5校验结果发送给发送端
6. **接收统计**：接收发送端的统计信息
7. **确认关闭**：发送关闭确认后关闭连接

### 关键信令：
- **握手包**：包含文件大小、MD5、MSS、窗口大小、文件名等信息
- **EOF标记**：`"FILE_TRANSMISSION_COMPLETE_EOF"`，表示数据传输完成
- **校验结果包**：包含MD5校验状态和实际接收字节数
- **关闭确认**：`"RECEIVER_CLOSE_OK"`，表示接收端准备关闭

## 🔧 高级配置

### CMake构建选项

| 选项 | 描述 | 可选值 | 默认值 |
|------|------|--------|--------|
| `DTARGET_OS` | 目标操作系统 | linux, windows | linux |
| `DCOMPILER_TARGET` | 目标架构 | x86, arm32, arm64 | x86 |
| `DTARGET_TYPE` | 目标类型 | executable, static, shared | executable |
| `DENABLE_OPENMP` | 启用OpenMP并行 | ON, OFF | OFF |
| `DPUBLIC_PACKAGE_DIR` | 外部包/库目录路径 | 路径或"NO" | NO |
| `DCMAKE_BUILD_TYPE` | 构建类型 | Release或Debug | Debug |

**构建示例：**
```bash
# ARM64架构
cmake .. -DCOMPILER_TARGET=arm64 -DTARGET_TYPE=executable

# Windows目标（使用MinGW）
cmake .. -DTARGET_OS=windows -DCOMPILER_TARGET=x86 -DPUBLIC_PACKAGE_DIR=/path/to/udt4/src -DTARGET_TYPE=executable -DCMAKE_BUILD_TYPE=Release

# 静态库构建
cmake .. -DTARGET_TYPE=static
```

### 传输参数调优

1. **MSS（最大分段大小）**
   - 建议值：1024-1500字节
   - 应根据网络MTU调整，避免IP分片
   - 局域网环境可用1500，广域网建议1024-1460

2. **窗口大小**
   - 默认：1MB（1048576字节）
   - 高延迟网络建议增大窗口
   - 公式：窗口大小 ≈ 带宽 × RTT
   - 示例：100Mbps带宽，50ms RTT，窗口 ≈ 0.625MB

3. **流控制**
   - 系统自动计算流控制窗口：`fc = (win_size / mss) × 2`
   - 最小值为25600个包

## 📊 统计信息说明

HRUFT提供全面的传输统计信息，帮助用户评估网络状况和传输效率。

### 实时进度报告（JSON格式）
```json
{
  "type": "progress",
  "percent": 65,
  "current": 697932185,
  "total": 1073741824,
  "speed_mbps": 2873.50,
  "elapsed_seconds": 2,
  "average_speed_mbps": 2500.18,
  "remaining_bytes": 375809639
}
```

### 完整统计报告
系统提供两种统计报告格式：
1. **JSON格式**：便于程序解析和集成
2. **文本格式**：便于人类阅读

#### 关键统计指标

**速度统计：**
- **平均速度**：整个传输过程的平均速度
- **最高速度**：传输过程中达到的最高瞬时速度
- **最低速度**：传输过程中的最低瞬时速度
- **当前速度**：最后一次测量的瞬时速度（使用3次滑动平均平滑）

**UDT原始包统计：**
- **总发送包数**：包括数据包和控制包
- **总接收包数**：包括数据包和控制包
- **数据包丢失数**：发送端和接收端分别统计
- **重传包数**：需要重传的数据包数
- **控制包统计**：ACK和NAK包的数量
- **估计数据包数**：基于文件大小和MSS计算的理论包数

**网络分析指标：**
- **数据包丢包率**：丢失的数据包比例（%）
- **控制包开销**：控制包占总体流量的比例（%）
- **网络传输效率**：有效数据传输比例（%）
- **数据完整性评分**：基于丢包率和重传率计算的评分（0-100）

### 网络质量评估
系统会根据统计数据自动评估网络质量并提供建议：

| 质量等级 | 丢包率 | 控制开销 | 说明 |
|----------|--------|----------|------|
| **Excellent** | < 2% | < 10% | 网络状况极佳，传输高效稳定 |
| **Good** | 2%-5% | < 20% | 网络状况良好，传输正常 |
| **Fair** | 5%-10% | < 30% | 网络状况一般，可能存在拥塞 |
| **Poor** | > 10% | > 30% | 网络状况较差，建议优化 |

**优化建议示例：**
```
High data loss (>10%). Consider reducing window size, increasing MSS, or checking network stability.
High control overhead. Consider adjusting UDT flow control parameters.
```

## 🏗️ 项目结构

```
hruft/
├── CMakeLists.txt          # CMake构建配置文件
├── README.md              # 项目说明文档
├── main.cpp              # 主程序文件（完整实现）
├── udt4/                  # UDT4网络库（需自行导入）
└── build/                # 构建目录
```

### 核心模块说明

#### 1. **传输统计模块** (`TransferStatistics`类)
**功能：**
- 实时速度计算（使用滑动平均平滑）
- UDT性能数据收集（通过`UDT::perfmon` API）
- 网络质量自动评估
- 统计数据序列化为JSON格式

**关键方法：**
- `init()` - 初始化统计
- `update_speed()` - 更新速度统计
- `collect_udt_stats()` - 收集UDT性能数据
- `calculate_derived_stats()` - 计算派生指标
- `to_json()` - 生成JSON统计报告

#### 2. **网络传输模块**
**功能：**
- UDT socket的创建、配置和管理
- 自适应参数调整（MSS、窗口大小）
- 可靠数据传输和重传
- 完整的握手和确认机制

**关键函数：**
- `apply_socket_opts()` - 配置socket参数
- `run_sender()` - 发送端主逻辑
- `run_receiver()` - 接收端主逻辑

#### 3. **文件处理模块**
**功能：**
- 大文件分块传输（64KB缓冲区）
- MD5校验计算（使用md5库）
- 文件信息和MIME类型检测
- 智能路径处理（自动创建目录）

**关键函数：**
- `calculate_file_md5()` - 计算文件MD5
- `get_mime_type_from_extension()` - 获取MIME类型
- `ensure_directory_exists()` - 确保目录存在

## 🔍 故障排除和优化

### 常见问题及解决方案

#### 1. **传输速度异常低**
**可能原因：**
- 窗口大小设置过小
- MSS值不合适
- 网络拥塞或丢包严重

**解决方案：**
```bash
# 增大窗口大小
hruft send 192.168.1.100 9000 file.iso --window 209715200

# 调整MSS值
hruft send 192.168.1.100 9000 file.iso --mss 1024

# 启用详细统计查看网络状况
hruft send 192.168.1.100 9000 file.iso --detailed
```

#### 2. **连接失败**
**检查步骤：**
1. 确认接收端已启动并监听
2. 检查防火墙设置（Windows防火墙、iptables）
3. 验证IP地址和端口号
4. 确认网络连通性（使用ping测试）

#### 3. **MD5校验失败**
**可能原因：**
- 网络传输过程中数据损坏
- 文件在发送端被修改
- 存储设备错误

**解决方案：**
1. 检查网络稳定性
2. 重新传输文件
3. 验证源文件完整性
4. 检查磁盘空间和权限

#### 4. **编译错误**
**常见问题：**
- UDT库未正确导入
- 编译器版本过低
- CMake配置错误

**解决方案：**
```bash
# 确保UDT库路径正确
cmake .. -DPUBLIC_PACKAGE_DIR=/path/to/udt4/src

# 升级编译器
sudo apt-get install g++-9  # Ubuntu

# 检查CMake版本
cmake --version  # 需要3.10+
```

### 性能优化建议

#### 对于高延迟网络（卫星、跨国链路）：
```bash
# 增大窗口大小以充分利用带宽
hruft send <ip> <port> <file> --window 524288000  # 500MB窗口

# 使用中等MSS值减少丢包影响
hruft send <ip> <port> <file> --mss 1024
```

#### 对于高带宽低延迟网络（局域网）：
```bash
# 使用最大MSS值提高效率
hruft send <ip> <port> <file> --mss 1500

# 适当增大窗口
hruft send <ip> <port> <file> --window 104857600  # 100MB窗口
```

#### 对于不稳定网络（无线、移动网络）：
```bash
# 减小MSS以降低丢包影响
hruft send <ip> <port> <file> --mss 512

# 使用中等窗口大小
hruft send <ip> <port> <file> --window 52428800  # 50MB窗口
```

## 🎯 最佳实践

### 1. **批量传输多个文件**
```bash
# 发送端脚本示例（Linux）
for file in /path/to/files/*.iso; do
    echo "传输: $file"
    ./hruft send 192.168.1.100 9000 "$file" --detailed
    sleep 1
done
```

### 2. **自动化监控集成**
HRUFT的JSON输出格式便于与监控系统集成：
```bash
# 将统计信息保存到文件
./hruft send 192.168.1.100 9000 file.iso --detailed 2>&1 | grep "{\"type\":\"statistics\"" > stats.json
```

### 3. **网络质量长期监控**
```bash
# 定期测试并记录网络状况
while true; do
    timestamp=$(date +%Y%m%d_%H%M%S)
    ./hruft send 192.168.1.100 9000 test_file.bin --detailed 2>&1 | tee "network_test_${timestamp}.log"
    sleep 300  # 每5分钟测试一次
done
```

## 📊 性能基准测试

### 典型性能指标（基于测试结果）

| 网络类型 | 平均速度 | 丢包率 | 推荐配置 |
|----------|----------|--------|----------|
| 千兆局域网 | 800-950 Mbps | < 0.5% | MSS=1500, Window=100MB |
| 百兆局域网 | 90-98 Mbps | < 1% | MSS=1500, Window=10MB |
| 高速互联网 | 50-200 Mbps | 1-5% | MSS=1024, Window=50MB |
| 高延迟卫星 | 10-50 Mbps | 5-10% | MSS=512, Window=20MB |

### 测试命令示例
```bash
# 创建测试文件（1GB）
dd if=/dev/zero of=test_1g.bin bs=1M count=1024

# 测试传输性能
./hruft send 192.168.1.100 9000 test_1g.bin --mss 1500 --window 104857600 --detailed
```

## 🆕 版本特性（最终版本）

### 已修复问题：
1. **超时误触发**：移除超时限制，支持大文件传输
2. **发送接收不同步**：实现完整的结束信令机制
3. **EOF标记处理**：改进EOF标记的检测和处理
4. **统计信息准确性**：优化速度计算算法

### 新增特性：
1. **智能EOF检测**：正确处理EOF标记与数据混合的情况
2. **双向统计同步**：发送端和接收端交换统计信息
3. **优雅关闭机制**：双方确认后有序关闭连接
4. **详细错误报告**：提供更清晰的错误信息和调试输出

## 🤝 贡献指南

### 提交问题
1. 在GitHub Issues中搜索相关问题
2. 如未找到，创建新Issue并详细描述问题
3. 包括以下信息：
   - 操作系统和版本
   - 编译器和版本
   - 详细的复现步骤
   - 相关日志输出

### 提交代码
1. Fork项目到个人账户
2. 创建功能分支：`git checkout -b feature/new-feature`
3. 提交更改：`git commit -m "描述变更内容"`
4. 推送到分支：`git push origin feature/new-feature`
5. 创建Pull Request

### 代码规范
- 遵循C++17标准
- 使用有意义的变量名和函数名
- 添加必要的注释（特别是复杂算法）
- 保持一致的代码风格（使用.clang-format）
- 添加单元测试（如果适用）

## 📄 许可证

本项目采用MIT许可证。详见[LICENSE](LICENSE)文件。

## 🙏 致谢

- **UDT项目团队**：提供高性能UDP传输协议基础
- **CMake社区**：优秀的跨平台构建系统
- **开源贡献者**：感谢每一位为项目做出贡献的人
- **测试用户**：提供宝贵的反馈和建议

## 📞 联系方式

- **项目维护者**：连思鑫
- **问题反馈**：[GitHub Issues](https://github.com/JinBiLianShao/HRUFT/issues)
- **邮箱**：2013182991@qq.com | jinbilianshao@gmail.com
- **项目地址**：https://github.com/JinBiLianShao/HRUFT

## 🔄 更新日志

### v1.0.0 (最终版本)
- ✅ 完整的传输信令机制
- ✅ 移除超时限制，支持大文件传输
- ✅ 改进的EOF标记处理
- ✅ 双向统计信息同步
- ✅ 优雅的连接关闭
- ✅ 详细的中文文档

### v0.9.0
- ✅ 基本文件传输功能
- ✅ MD5完整性校验
- ✅ 详细统计信息
- ✅ 跨平台支持

---

## ⚡ 快速参考表

### 常用命令
| 命令 | 说明 |
|------|------|
| `hruft send <ip> <port> <file> --detailed` | 发送文件并显示详细统计 |
| `hruft recv <port> <dir/> --detailed` | 接收文件到目录 |
| `hruft recv <port> <file> --detailed` | 接收文件到指定路径 |

### 优化参数
| 场景 | 推荐配置 |
|------|----------|
| 千兆局域网 | `--mss 1500 --window 100MB` |
| 高延迟网络 | `--mss 1024 --window 50MB` |
| 不稳定网络 | `--mss 512 --window 20MB` |

### 关键统计指标
| 指标 | 理想值 | 说明 |
|------|--------|------|
| 丢包率 | < 2% | 网络状况良好 |
| 控制开销 | < 10% | 控制包开销合理 |
| 传输效率 | > 95% | 数据传输高效 |
| 完整性评分 | > 90 | 数据完整性好 |

### 调试信息
| 信息类型 | 说明 |
|----------|------|
| `{"type":"progress"}` | 实时进度报告 |
| `{"type":"statistics"}` | 完整统计信息 |
| `{"type":"verify"}` | MD5校验结果 |
| `{"type":"status"}` | 状态信息 |

---

## 💡 使用技巧

1. **首次使用时**：先用小文件测试，确认连接正常
2. **性能调优**：根据统计信息调整MSS和窗口大小
3. **批量传输**：编写脚本实现自动化批量传输
4. **监控集成**：利用JSON输出集成到现有监控系统
5. **网络测试**：使用HRUFT作为网络质量测试工具

---

## ⚠️ 注意事项

1. **网络环境**：本系统专为可控网络环境设计，不适用于需要NAT穿透的场景
2. **大文件传输**：确保接收端有足够的磁盘空间
3. **权限问题**：Linux环境下注意文件读写权限
4. **防火墙**：确保相关端口在防火墙中开放
5. **内存使用**：大窗口设置会占用较多内存，请根据系统资源调整

---

**重要提示**：对于超大规模文件传输（>100GB），建议先进行小规模测试，确认网络稳定性和系统资源充足。

**性能提示**：传输速度受限于网络带宽、延迟、丢包率和两端系统性能，请根据实际情况调整参数。

---

⭐ **如果这个项目对你有帮助，请给它一个Star！** ⭐

📢 **欢迎提交Issue和Pull Request，共同改进HRUFT！**