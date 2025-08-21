## 🛠️ Hardware Requirements

### Core Components
- **Microcontroller**: ESP32 WROOM-32D or ESP32-C3
- **Display**: SSD1306 OLED (128x64, I2C interface)
- **NFC Module**: PN532 (I2C interface)
- **Light Sensor**: Light Dependent Resistor (LDR)
- **Audio**: Buzzer module

### Wiring Diagram

```
ESP32 WROOM-32D:
├── OLED SSD1306 (128x64)
│   ├── SDA → GPIO 21
│   ├── SCL → GPIO 22
│   ├── VCC → 3.3V
│   └── GND → GND
├── PN532 NFC Module
│   ├── SDA → GPIO 21 (Shared I2C bus with OLED)
│   ├── SCL → GPIO 22 (Shared I2C bus with OLED)
│   ├── VCC → 3.3V
│   └── GND → GND
├── Light Dependent Resistor (LDR)
│   ├── One end → GPIO 36 (ADC input)
│   ├── Other end → 3.3V
│   └── Pull-down resistor 10kΩ → GND
├── Buzzer Module
│   ├── VCC → 3.3V
│   ├── GND → GND
│   └── Signal → GPIO 5
└── Wake-up Button (Optional)
    ├── One end → GPIO 33
    └── Other end → GND
```

## 🚀 Quick Start

### Environment Setup

1. **Install Arduino IDE** (Recommended version 2.0+)
2. **Add ESP32 Board Support**:
   - File → Preferences → Additional Boards Manager URLs
   - Add: `https://dl.espressif.com/dl/package_esp32_index.json`
   - Tools → Board → Boards Manager → Search "ESP32" and install

3. **Install Required Libraries**:
   ```
   - Adafruit GFX Library
   - Adafruit SSD1306
   - Adafruit PN532
   - ESP32 Preferences
   ```

### Compile and Upload

1. **Select Board**:
   - ESP32 WROOM-32D: `ESP32 Dev Module`
   - ESP32-C3: `ESP32C3 Dev Module`

2. **Configure Parameters**:
   ```
   Upload Speed: 921600
   CPU Frequency: 240MHz (ESP32) / 160MHz (ESP32-C3)
   Flash Mode: DIO
   Flash Size: 4MB
   Partition Scheme: Default 4MB
   ```

3. **Choose Version**:
   - `bookstand v3/bookstand_WROOM_withbetterNFCmanagement/` - ESP32 WROOM-32D version
   - `bookstand v3/WROOM32Ddeepsleepmode/` - Deep sleep optimized version
   - `bookstand v3/finalOLEDtest/finalOLEDv3/` - ESP32-C3 version

## 📋 Functionality

### NFC Book Management
- Place NFC card above the device for automatic book recognition
- First-time use automatically registers new books
- Support for custom book names and information
- Independent timing and statistics for each book

### Reading Detection
- Automatic reading state detection via light sensor
- Smart filtering of ambient light interference
- Adaptive threshold adjustment
- Real-time status feedback

### Pomodoro Timer
- Automatically adjusts duration based on personal reading habits
- Audio notification when timer expires
- Smart break time suggestions
- Focus concentration statistics

### Data Statistics
- Total reading time for each book
- Average reading session duration
- Reading frequency analysis
- Number of completed Pomodoro sessions

## 🔧 Advanced Configuration

### Sensor Calibration

Modify in `bookstand_WROOM_withbetterNFCmanagement.ino`:

```cpp
// Light threshold configuration
const int baselineThreshold = 500;    // Base light threshold
const int dynamicRange = 200;         // Dynamic range
const int noiseMargin = 50;           // Noise margin

// Sleep configuration
const unsigned long idleTimeout = 30000;        // Idle timeout (milliseconds)
const unsigned long lowLightSleepDelay = 2000;  // Low light delay (milliseconds)
```

### Book Management

```cpp
#define MAX_BOOKS 10              // Maximum number of books
#define BOOK_NAME_MAX_LENGTH 20   // Maximum book name length
```

### Deep Sleep Optimization

Enable deep sleep functionality to reduce standby power consumption to 10μA:

```cpp
// In WROOM32Ddeepsleepmode version
const unsigned long wakeupInterval = 60000;  // Wake-up interval (milliseconds)
```

## 📊 Version History

### v3.0 - Multi-Book Management Version
- ✅ Support for tracking up to 10 books independently
- ✅ Improved NFC management system
- ✅ Book statistics data persistence
- ✅ Smart book switching notifications

### v2.3 - Deep Sleep Optimization
- ✅ True deep sleep mode (10μA power consumption)
- ✅ Smart timer wake-up
- ✅ Adaptive wake-up intervals
- ✅ GPIO wake-up support

### v2.0 - NFC Multi-Book Support
- ✅ NFC card book recognition
- ✅ Independent timing for multiple books
- ✅ OLED pixel art interface
- ✅ I2C bus conflict optimization

## 🐛 Troubleshooting

### Common Issues

**Q: OLED display not working**
- Check I2C connection wires are correct
- Confirm OLED address is 0x3C
- Check power supply is normal

**Q: NFC cannot read cards**
- Confirm PN532 module I2C mode jumper settings
- Check for I2C address conflicts with OLED
- Try different types of NFC cards

**Q: Light detection inaccurate**
- Adjust LDR position to avoid obstruction
- Modify light threshold parameters in code
- Confirm pull-down resistor value is 10kΩ

**Q: Cannot wake up from deep sleep**
- Check GPIO33 pin connection# 📚 Smart Reading Tracker

An intelligent reading time tracking device based on ESP32 that automatically recognizes different books through NFC cards, records reading time, and provides Pomodoro timer, statistical analysis, and other features.

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Platform](https://img.shields.io/badge/platform-ESP32-green.svg)
![Version](https://img.shields.io/badge/version-v3.0-orange.svg)

## ✨ Key Features

### 🔖 Multi-Book Management
- Support for tracking up to 10 books independently
- Automatic book recognition and switching via NFC cards
- Individual reading statistics for each book
- Persistent data storage, no data loss on power off

### 📊 Smart Tracking
- Automatic reading state detection via light sensor
- Real-time reading time recording
- Adaptive Pomodoro timer functionality
- Personalized reading habit analysis

### 💡 Power-Efficient Design
- Smart deep sleep mode (only 10μA power consumption)
- Adaptive wake-up intervals (1-5 minutes)
- Dual wake-up mechanism: GPIO and timer
- Intelligent I2C bus management

### 🎨 Beautiful Interface
- 128x64 OLED display
- Pixel art style UI design
- Dynamic wave animation effects
- Real-time status indicators

## 🛠️ 硬件要求

### 核心组件
- **主控板**: ESP32 WROOM-32D 或 ESP32-C3
- **显示屏**: SSD1306 OLED (128x64, I2C接口)
- **NFC模块**: PN532 (I2C接口)
- **光线传感器**: 光敏电阻 (LDR)
- **音响**: 蜂鸣器模块

### 连接方式

```
ESP32 WROOM-32D:
├── OLED SSD1306 (128x64)
│   ├── SDA → GPIO 21
│   ├── SCL → GPIO 22
│   ├── VCC → 3.3V
│   └── GND → GND
├── PN532 NFC模块
│   ├── SDA → GPIO 21 (与OLED共享I2C总线)
│   ├── SCL → GPIO 22 (与OLED共享I2C总线)
│   ├── VCC → 3.3V
│   └── GND → GND
├── 光敏电阻 (LDR)
│   ├── 一端 → GPIO 36 (ADC输入)
│   ├── 另一端 → 3.3V
│   └── 下拉电阻10kΩ → GND
├── 蜂鸣器模块
│   ├── VCC → 3.3V
│   ├── GND → GND
│   └── 信号线 → GPIO 5
└── 唤醒按钮 (可选)
    ├── 一端 → GPIO 33
    └── 另一端 → GND
```

## 🚀 快速开始

### 环境准备

1. **安装Arduino IDE** (推荐版本 2.0+)
2. **添加ESP32开发板支持**:
   - 文件 → 首选项 → 附加开发板管理器网址
   - 添加: `https://dl.espressif.com/dl/package_esp32_index.json`
   - 工具 → 开发板 → 开发板管理器 → 搜索"ESP32"并安装

3. **安装必需库**:
   ```
   - Adafruit GFX Library
   - Adafruit SSD1306
   - Adafruit PN532
   - ESP32 Preferences
   ```

### 编译上传

1. **选择开发板**:
   - ESP32 WROOM-32D: `ESP32 Dev Module`
   - ESP32-C3: `ESP32C3 Dev Module`

2. **配置参数**:
   ```
   Upload Speed: 921600
   CPU Frequency: 240MHz (ESP32) / 160MHz (ESP32-C3)
   Flash Mode: DIO
   Flash Size: 4MB
   Partition Scheme: Default 4MB
   ```

3. **选择版本**:
   - `bookstand v3/bookstand_WROOM_withbetterNFCmanagement/` - ESP32 WROOM-32D版本
   - `bookstand v3/WROOM32Ddeepsleepmode/` - 深度睡眠优化版本
   - `bookstand v3/finalOLEDtest/finalOLEDv3/` - ESP32-C3版本

## 📋 功能说明

### NFC书籍管理
- 将NFC卡片放在设备上方即可自动识别书籍
- 首次使用会自动注册新书籍
- 支持自定义书籍名称和信息
- 每本书籍独立计时和统计

### 阅读检测
- 光线传感器自动检测是否在阅读
- 智能过滤环境光干扰
- 自适应阈值调整
- 实时状态反馈

### 番茄钟功能
- 根据个人阅读习惯自动调整时长
- 到时提醒音效
- 休息时间智能建议
- 专注度统计分析

### 数据统计
- 每本书的总阅读时间
- 平均阅读时长
- 阅读频率分析
- 番茄钟完成数量

## 🔧 高级配置

### 传感器调校

在 `bookstand_WROOM_withbetterNFCmanagement.ino` 中修改:

```cpp
// 光线阈值配置
const int baselineThreshold = 500;    // 基础光线阈值
const int dynamicRange = 200;         // 动态范围
const int noiseMargin = 50;           // 噪声容限

// 睡眠配置
const unsigned long idleTimeout = 30000;        // 空闲超时 (毫秒)
const unsigned long lowLightSleepDelay = 2000;  // 低光延迟 (毫秒)
```

### 书籍管理

```cpp
#define MAX_BOOKS 10              // 最大书籍数量
#define BOOK_NAME_MAX_LENGTH 20   // 书名最大长度
```

### 深度睡眠优化

启用深度睡眠功能可将待机功耗降至10μA:

```cpp
// 在 WROOM32Ddeepsleepmode 版本中
const unsigned long wakeupInterval = 60000;  // 唤醒间隔 (毫秒)
```

## 📊 版本历史

### v3.0 - 多书籍管理版本
- ✅ 支持最多10本书籍独立追踪
- ✅ 改进的NFC管理系统
- ✅ 书籍统计数据持久化
- ✅ 智能书籍切换提示

### v2.3 - 深度睡眠优化
- ✅ 真正的深度睡眠模式 (10μA功耗)
- ✅ 智能定时器唤醒
- ✅ 自适应唤醒间隔
- ✅ GPIO唤醒支持

### v2.0 - NFC多书籍支持
- ✅ NFC卡片书籍识别
- ✅ 多书籍独立计时
- ✅ OLED像素艺术界面
- ✅ I2C总线冲突优化

## 🐛 故障排除

### 常见问题

**Q: OLED显示屏不亮**
- 检查I2C连接线是否正确
- 确认OLED地址为0x3C
- 检查电源供电是否正常

**Q: NFC无法读取**
- 确认PN532模块I2C模式跳线设置
- 检查与OLED是否存在I2C地址冲突
- 尝试不同类型的NFC卡片

**Q: 光线检测不准确**
- 调整光敏电阻位置避免遮挡
- 修改代码中的光线阈值参数
- 确认下拉电阻值为10kΩ

**Q: 深度睡眠后无法唤醒**
- 检查GPIO33引脚连接
- 确认上拉电阻配置正确
- 验证深度睡眠唤醒源设置

### 调试模式

启用串口调试信息:
```cpp
Serial.begin(115200);
// 在setup()中添加调试输出
Serial.println("=== Debug Mode Enabled ===");
```

## 🤝 贡献指南

欢迎提交Issue和Pull Request！

### 开发环境
- Arduino IDE 2.0+
- ESP32 Arduino Core 2.0+
- Git版本控制

### 提交规范
- 功能改进: `feat: 添加新功能描述`
- 问题修复: `fix: 修复问题描述`
- 文档更新: `docs: 更新文档内容`

## 📄 许可证

本项目采用 MIT 许可证 - 查看 [LICENSE](LICENSE) 文件了解详情。

## 👨‍💻 作者

**Zhiyu Cao** - 嵌入式开发者和阅读爱好者

## 🙏 致谢

- Adafruit - 优秀的硬件库支持
- ESP32社区 - 丰富的开发资源
- 所有测试用户的宝贵反馈

---

**📖 享受智能阅读时光！如果这个项目对您有帮助，请给我们一个⭐️**
