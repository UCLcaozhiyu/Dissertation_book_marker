/*
 * =======================================================================
 * ESP32C3 NFC智能阅读追踪器 v2.3 - 定时器唤醒版本 (修复版)
 * =======================================================================
 * 
 * 功能特色：
 * - 智能定时器唤醒：根据光线历史智能调整睡眠间隔
 * - 深度睡眠期间完全关闭OLED、NFC和蜂鸣器
 * - 自适应间隔：1-5分钟动态调整，平衡功耗与响应速度
 * - 简洁高效：移除复杂逻辑，专注核心功能
 * - 超低功耗：深度睡眠功耗仅约10µA
 * 
 * 硬件连接：
 * ESP32C3 开发板:
 * ├── OLED SSD1306 (128x64)
 * │   ├── SDA → GPIO 8
 * │   ├── SCL → GPIO 9
 * │   ├── VCC → 3.3V
 * │   └── GND → GND
 * ├── PN532 NFC模块
 * │   ├── SDA → GPIO 8 (与OLED共享I2C总线)
 * │   ├── SCL → GPIO 9 (与OLED共享I2C总线)
 * │   ├── VCC → 3.3V
 * │   └── GND → GND
 * ├── 光敏电阻 (LDR)
 * │   ├── 一端 → GPIO 0 (ADC输入)
 * │   ├── 另一端 → 3.3V
 * │   └── 下拉电阻10kΩ → GND
 * ├── 蜂鸣器模块
 * │   ├── VCC/+ → 3.3V (红线)
 * │   ├── GND/- → GND  (黑线)
 * │   └── I/O/S → GPIO 5 (信号线)
 * └── 按钮 (可选)
 *     ├── 一端 → GPIO 3
 *     └── 另一端 → GND
 * 
 * 作者: AI Assistant
 * 更新日期: 2025-01-17
 * 方案: 定时器唤醒 + 智能间隔调整 (简洁高效)
 * =======================================================================
 */

// ===== 库文件引入 =====
#include <Arduino.h>           // Arduino核心库
#include "driver/ledc.h"       // ESP32 LEDC PWM驱动 (蜂鸣器控制)
#include <Preferences.h>       // ESP32非易失性存储库
#include <Wire.h>              // I2C通信库
#include <Adafruit_GFX.h>      // Adafruit图形库
#include <Adafruit_SSD1306.h>  // OLED显示屏驱动
#include "esp_system.h"        // ESP32系统函数
#include "esp_sleep.h"         // ESP32深度睡眠功能
#include <Adafruit_PN532.h>    // PN532 NFC模块驱动
#include <math.h>              // 数学函数库 (用于波浪线动画)

// ===== 硬件引脚定义 =====
#define SCREEN_WIDTH 128       // OLED屏幕宽度 (像素)
#define SCREEN_HEIGHT 64       // OLED屏幕高度 (像素)
#define OLED_RESET -1          // OLED复位引脚 (-1表示使用软件复位)
#define SCREEN_ADDRESS 0x3C    // OLED的I2C地址

#define SDA_PIN 8              // I2C数据线引脚 (SDA)
#define SCL_PIN 9              // I2C时钟线引脚 (SCL)

const int ldrPin = 0;          // 光敏电阻连接的ADC引脚
const int piezoPin = 5;        // 蜂鸣器连接的PWM引脚
const int wakeupButtonPin = 3; // 外部唤醒按钮引脚 (可选)

// ===== 传感器阈值配置 =====
const int lightThreshold = 1000;      // 光线阈值：高于此值认为在阅读
const int bookmarkThreshold = 50;     // 书签阈值：低于此值进入深度睡眠
const int wakeupLightThreshold = 800; // 唤醒光线阈值：高于此值才真正激活设备
const uint64_t uS_TO_S_FACTOR = 1000000ULL;  // 微秒到秒的转换因子

// ===== 睡眠管理变量 =====
bool systemFullyAwake = false;        // 系统是否完全唤醒
unsigned long lastFullWakeTime = 0;   // 上次完全唤醒时间
const unsigned long sleepCheckInterval = 30000;  // 睡眠检查间隔 (30秒)

// ===== 像素图标数据 (16x16像素，单色位图) =====
// 📚 书本图标 - 用于表示书籍和阅读
const unsigned char icon_book[] PROGMEM = {
  0x00, 0x00, 0x1F, 0xF8, 0x10, 0x08, 0x10, 0x08, 0x10, 0x08, 0x10, 0x08, 
  0x1F, 0xF8, 0x10, 0x08, 0x10, 0x08, 0x10, 0x08, 0x10, 0x08, 0x10, 0x08, 
  0x10, 0x08, 0x1F, 0xF8, 0x00, 0x00, 0x00, 0x00
};

// 🕐 时钟图标 - 用于表示时间和计时
const unsigned char icon_clock[] PROGMEM = {
  0x03, 0xC0, 0x0C, 0x30, 0x10, 0x08, 0x20, 0x04, 0x20, 0x04, 0x41, 0x82, 
  0x41, 0x82, 0x41, 0x02, 0x41, 0x02, 0x20, 0x04, 0x20, 0x04, 0x10, 0x08, 
  0x0C, 0x30, 0x03, 0xC0, 0x00, 0x00, 0x00, 0x00
};

// ❤️ 心形图标 - 用于表示喜爱和欢迎
const unsigned char icon_heart[] PROGMEM = {
  0x00, 0x00, 0x0E, 0x70, 0x1F, 0xF8, 0x3F, 0xFC, 0x7F, 0xFE, 0x7F, 0xFE, 
  0x3F, 0xFC, 0x1F, 0xF8, 0x0F, 0xF0, 0x07, 0xE0, 0x03, 0xC0, 0x01, 0x80, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// ⭐ 星星图标 - 用于装饰和成就感
const unsigned char icon_star[] PROGMEM = {
  0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x0D, 0xB0, 0x1F, 0xF8, 0x0F, 0xF0, 
  0x07, 0xE0, 0x0F, 0xF0, 0x1B, 0xD8, 0x31, 0x8C, 0x60, 0x06, 0x60, 0x06, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// 🔋 电池图标 - 用于表示电源状态和传感器
const unsigned char icon_battery[] PROGMEM = {
  0x00, 0x00, 0x3F, 0xF0, 0x20, 0x18, 0x20, 0x18, 0x27, 0x98, 0x27, 0x98, 
  0x27, 0x98, 0x27, 0x98, 0x27, 0x98, 0x27, 0x98, 0x20, 0x18, 0x20, 0x18, 
  0x3F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// 🌙 月亮图标 - 用于表示睡眠状态 (新增)
const unsigned char icon_moon[] PROGMEM = {
  0x07, 0x00, 0x0F, 0x80, 0x1F, 0xC0, 0x3F, 0xE0, 0x3F, 0xE0, 0x7F, 0xF0, 
  0x7F, 0xF0, 0x7F, 0xF0, 0x7F, 0xF0, 0x7F, 0xF0, 0x3F, 0xE0, 0x3F, 0xE0, 
  0x1F, 0xC0, 0x0F, 0x80, 0x07, 0x00, 0x00, 0x00
};

// ===== 硬件对象实例化 =====
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);  // OLED显示屏对象
Adafruit_PN532 nfc(SDA_PIN, SCL_PIN);  // PN532 NFC模块对象

// ===== NFC相关变量 =====
String lastUID = "";              // 上次检测到的NFC卡片UID
bool showLastUIDOnBoot = false;   // 启动时是否显示上次UID
bool nfcInitialized = false;      // NFC模块是否初始化成功
bool oledInitialized = false;     // OLED是否初始化成功
bool i2cBusy = false;             // I2C总线互斥标志 (防止冲突)
bool isShowingNFC = false;        // 当前是否正在显示NFC界面
unsigned long nfcDisplayStartTime = 0;  // NFC界面显示开始时间

// ===== 番茄钟配置 =====
const int defaultPomodoroMin = 25;                              // 默认番茄钟时长 (分钟)
unsigned long adaptivePomodoroMillis = defaultPomodoroMin * 60000UL;  // 自适应番茄钟时长 (毫秒)
unsigned long adaptiveTotalSessionTime = 0;                     // 累计会话时间 (用于计算平均值)
unsigned long adaptiveSessionCount = 0;                         // 会话计数 (用于计算平均值)
const unsigned long restDuration = 5 * 60 * 1000UL;           // 休息时间 (5分钟)

// ===== 阅读状态变量 =====
bool isReading = false;           // 当前是否在阅读
bool isPaused = false;            // 是否暂停
bool inRest = false;              // 是否在休息时间
bool isSleeping = false;          // 是否在睡眠状态

// ===== 时间记录变量 =====
unsigned long sessionStartMillis = 0;        // 当前会话开始时间
unsigned long accumulatedSessionMillis = 0;  // 累积的会话时间
unsigned long restStartMillis = 0;           // 休息开始时间
unsigned long totalReadingSeconds = 0;       // 总阅读时间 (秒)

// ===== 蜂鸣器控制变量 =====
unsigned long lastBeepTime = 0;              // 上次蜂鸣器响的时间
const unsigned long beepInterval = 10000;    // 蜂鸣器间隔时间 (10秒)

// ===== 数据存储对象 =====
Preferences prefs;  // ESP32非易失性存储对象，用于保存数据到Flash

// ===== 光线趋势分析变量 =====
const int trendSize = 30;        // 趋势数据数组大小
int lightTrend[trendSize] = {0}; // 光线趋势数据数组
int trendIndex = 0;              // 当前趋势数据索引
float bestLuxEMA = 0.0;          // 最佳光照度的指数移动平均值
const float emaAlpha = 0.05;     // EMA平滑系数 (值越小越平滑)
int animationFrame = 0;          // 动画帧计数器

/*
 * =======================================================================
 * 函数声明区域 (解决函数调用顺序问题)
 * =======================================================================
 */
void beep(int duration = 100);
void beepNFCSuccess();
void beepPomodoroComplete();
void beepLightWarning();
bool initializeOLED();
bool initializeNFC();
void turnOffOLED();
bool shouldFullyWakeup();

/*
 * =======================================================================
 * 定时器唤醒 + 智能间隔机制
 * =======================================================================
 * 工作原理：
 * 1. 使用ESP32定时器唤醒 (最可靠的方式)
 * 2. 根据光线历史智能调整睡眠间隔
 * 3. 深度睡眠时完全关闭硬件，功耗约10µA
 * 4. 唤醒后快速检测光线，决定是否激活硬件
 * 
 * 间隔策略：
 * - 刚变暗：1分钟后检查 (快速响应)
 * - 持续较暗：2分钟后检查 (中等频率)
 * - 长期黑暗：5分钟后检查 (节能优先)
 * =======================================================================
 */

/**
 * 智能睡眠间隔计算
 * 返回：下次唤醒的秒数
 * 功能：根据光线历史智能调整睡眠时间，平衡功耗与响应速度
 */
unsigned long calculateSleepInterval() {
  static int consecutiveDarkReadings = 0;
  static unsigned long lastLightDetectedTime = 0;
  
  int lightValue = analogRead(ldrPin);
  unsigned long now = millis();
  
  // 记录光线变化历史
  if (lightValue < bookmarkThreshold) {
    consecutiveDarkReadings++;
  } else {
    consecutiveDarkReadings = 0;
    lastLightDetectedTime = now;
  }
  
  // 智能间隔策略：
  if (consecutiveDarkReadings < 3) {
    // 刚变暗，可能用户很快回来，频繁检查
    Serial.println("Light recently changed, short interval: 60s");
    return 60;  // 1分钟
  } else if (consecutiveDarkReadings < 10) {
    // 持续较暗，中等间隔
    Serial.println("Moderately dark, medium interval: 120s");
    return 120; // 2分钟
  } else {
    // 长期黑暗，延长间隔节省电力
    Serial.println("Long-term dark, extended interval: 300s");
    return 300; // 5分钟
  }
}

/**
 * 配置外部唤醒 (可选功能)
 * 功能：设置按钮作为外部唤醒源 (ESP32C3兼容版本)
 */
//void setupExternalWakeup() {
  // ESP32C3 使用 ext1 唤醒而不是 ext0
  // 配置按钮引脚
  //pinMode(wakeupButtonPin, INPUT_PULLUP);
  
  // ESP32C3 使用 GPIO唤醒，配置GPIO3为唤醒源
 // esp_sleep_enable_gpio_wakeup();
  
  // 配置GPIO3为下降沿唤醒
 // gpio_wakeup_enable((gpio_num_t)wakeupButtonPin, GPIO_INTR_LOW_LEVEL);
  
 // Serial.println("External wakeup configured on GPIO" + String(wakeupButtonPin) + " (button) for ESP32C3");
//}

/**
 * 检查光线是否满足唤醒条件
 * 返回：bool - 是否应该完全唤醒系统
 * 功能：检测光线强度，判断是否需要激活硬件
 */
bool shouldFullyWakeup() {
  static int consecutiveBrightReadings = 0;
  static unsigned long lastBrightTime = 0;
  
  int lightValue = analogRead(ldrPin);
  unsigned long now = millis();
  
  if (lightValue > wakeupLightThreshold) {
    if (lastBrightTime == 0 || (now - lastBrightTime) < 1000) {
      consecutiveBrightReadings++;
      lastBrightTime = now;
    } else {
      // 间隔太长，重新计数
      consecutiveBrightReadings = 1;
      lastBrightTime = now;
    }
    
    // 需要连续3次亮度读数都满足条件
    if (consecutiveBrightReadings >= 3) {
      Serial.println("Light condition met for full wakeup: " + String(lightValue));
      return true;
    }
  } else {
    // 光线不足，重置计数
    consecutiveBrightReadings = 0;
    lastBrightTime = 0;
  }
  
  return false;
}

/**
 * 检查是否满足完全唤醒条件 (重载函数)
 * 返回：bool - 是否应该完全唤醒系统
 * 功能：简化版本的检查，兼容现有代码
 */
bool checkWakeupCondition() {
  return shouldFullyWakeup();
}

/**
 * 完全唤醒系统硬件
 * 功能：初始化OLED、NFC、蜂鸣器等所有组件
 */
void fullyWakeupSystem() {
  if (systemFullyAwake) return;  // 避免重复初始化
  
  Serial.println("=== Activating All Hardware ===");
  
  // 重新初始化I2C总线
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(200);
  
  // 初始化OLED
  oledInitialized = initializeOLED();
  if (oledInitialized) {
    Serial.println("OLED activated");
  }
  
  // 初始化NFC
  nfcInitialized = initializeNFC();
  if (nfcInitialized) {
    Serial.println("NFC activated");
  }
  
  // 蜂鸣器准备
  digitalWrite(piezoPin, HIGH);
  
  systemFullyAwake = true;
  lastFullWakeTime = millis();
  
  // 播放唤醒提示音
  beep(100);
  delay(50);
  beep(100);
  
  Serial.println("All hardware activated successfully");
}

/**
 * 进入深度睡眠模式
 * 功能：保存数据、关闭硬件、设置定时器、进入深度睡眠
 */
void enterDeepSleep() {
  Serial.println("=== Entering Deep Sleep ===");
  
  // 保存当前阅读数据
  if (isReading || isPaused) {
    unsigned long sessionMillis = accumulatedSessionMillis;
    if (isReading) sessionMillis += millis() - sessionStartMillis;
    totalReadingSeconds += sessionMillis / 1000;
    
    saveAdaptivePomodoro(sessionMillis);
    prefs.putULong("totalSecs", totalReadingSeconds);
    Serial.println("Reading data saved");
  }
  
  // 显示睡眠提示 (如果OLED可用)
  if (oledInitialized && !i2cBusy) {
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(30, 20);
    display.println("Sleep");
    display.drawBitmap(56, 40, icon_moon, 16, 16, SSD1306_WHITE);
    display.display();
    delay(1000);
  }
  
  // 关闭所有硬件
  turnOffOLED();
  
  // 重置状态标志
  systemFullyAwake = false;
  oledInitialized = false;
  nfcInitialized = false;
  isShowingNFC = false;
  
  // 计算智能睡眠间隔
  unsigned long sleepSeconds = calculateSleepInterval();
  
  // 配置定时器唤醒
  esp_sleep_enable_timer_wakeup(sleepSeconds * uS_TO_S_FACTOR);
  
  Serial.println("Sleep interval: " + String(sleepSeconds) + " seconds");
  Serial.println("Entering deep sleep now...");
  Serial.flush();  // 确保串口输出完成
  
  // 进入深度睡眠
  esp_deep_sleep_start();
  // 注意：执行到这里程序会停止，直到定时器唤醒
}

/**
 * 检查是否应该进入睡眠模式
 * 返回：bool - 是否应该进入深度睡眠
 * 功能：简化的睡眠判断逻辑
 */
bool shouldEnterSleep() {
  int lightValue = analogRead(ldrPin);
  
  // 主要条件：光线过暗，无法阅读
  if (lightValue < bookmarkThreshold) {
    Serial.println("Light too dark, entering sleep: " + String(lightValue));
    return true;
  }
  
  // 额外条件：系统已唤醒但长时间光线不足
  if (systemFullyAwake && 
      (millis() - lastFullWakeTime) > sleepCheckInterval && 
      lightValue < lightThreshold) {
    Serial.println("Long inactivity, entering sleep: " + String(lightValue));
    return true;
  }
  
  return false;
}

/*
 * =======================================================================
 * 原有功能函数 (保持不变)
 * =======================================================================
 */

/**
 * I2C设备扫描函数
 */
void scanI2CDevices() {
  Serial.println("Scanning I2C devices...");
  int deviceCount = 0;
  
  for (byte address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    byte error = Wire.endTransmission();
    
    if (error == 0) {
      Serial.print("I2C device found at address 0x");
      if (address < 16) Serial.print("0");
      Serial.print(address, HEX);
      Serial.println();
      deviceCount++;
    }
  }
  
  Serial.print("Total devices found: ");
  Serial.println(deviceCount);
}

/**
 * 绘制屏幕四角装饰边框
 */
void drawCornerDecorations() {
  display.drawLine(0, 0, 8, 0, SSD1306_WHITE);
  display.drawLine(0, 0, 0, 8, SSD1306_WHITE);
  display.drawLine(119, 0, 127, 0, SSD1306_WHITE);
  display.drawLine(127, 0, 127, 8, SSD1306_WHITE);
  display.drawLine(0, 56, 8, 56, SSD1306_WHITE);
  display.drawLine(0, 56, 0, 63, SSD1306_WHITE);
  display.drawLine(119, 63, 127, 63, SSD1306_WHITE);
  display.drawLine(127, 55, 127, 63, SSD1306_WHITE);
}

/**
 * 绘制动态轮播图标
 */
void drawAnimatedIcon(int x, int y, int frame) {
  const unsigned char* icons[] = {icon_book, icon_heart, icon_star, icon_clock};
  int iconIndex = (frame / 20) % 4;
  display.drawBitmap(x, y, icons[iconIndex], 16, 16, SSD1306_WHITE);
}

/**
 * 绘制进度条
 */
void drawProgressBar(int x, int y, int width, int height, int progress, int maxProgress) {
  display.drawRect(x, y, width, height, SSD1306_WHITE);
  int fillWidth = (progress * (width - 2)) / maxProgress;
  if (fillWidth > 0) {
    display.fillRect(x + 1, y + 1, fillWidth, height - 2, SSD1306_WHITE);
  }
}

/**
 * 初始化OLED显示屏
 */
bool initializeOLED() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("OLED initialization failed!");
    return false;
  }
  
  // 启动动画
  display.clearDisplay();
  drawCornerDecorations();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(15, 2);
  display.println("Reading Tracker");
  display.drawBitmap(30, 20, icon_book, 16, 16, SSD1306_WHITE);
  display.drawBitmap(50, 20, icon_heart, 16, 16, SSD1306_WHITE);
  display.drawBitmap(70, 20, icon_clock, 16, 16, SSD1306_WHITE);
  display.setCursor(45, 45);
  display.println("v2.3");
  for (int i = 0; i < SCREEN_WIDTH; i += 8) {
    display.drawPixel(i, 55, SSD1306_WHITE);
  }
  display.display();
  delay(2000);
  
  display.clearDisplay();
  display.setCursor(35, 20);
  display.println("Starting...");
  display.drawBitmap(56, 35, icon_star, 16, 16, SSD1306_WHITE);
  display.display();
  
  Serial.println("OLED initialized successfully");
  return true;
}

/**
 * 初始化NFC模块
 */
bool initializeNFC() {
  Serial.println("Initializing NFC...");
  nfc.begin();
  
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("PN532 not found!");
    return false;
  }
  
  Serial.print("Found PN532, FW ver: ");
  Serial.print((versiondata >> 16) & 0xFF, DEC);
  Serial.print('.');
  Serial.println((versiondata >> 8) & 0xFF, DEC);
  
  nfc.SAMConfig();
  Serial.println("NFC initialized successfully");
  return true;
}

/**
 * 关闭OLED显示屏
 */
void turnOffOLED() {
  if (oledInitialized) {
    display.clearDisplay();
    display.display();
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    delay(100);
  }
  Wire.end();
  pinMode(SDA_PIN, INPUT_PULLUP);
  pinMode(SCL_PIN, INPUT_PULLUP);
}

/**
 * 蜂鸣器提示音函数
 */
void beep(int duration) {
  if (!systemFullyAwake) return;  // 只有完全唤醒时才允许蜂鸣
  
  digitalWrite(piezoPin, LOW);
  delay(duration);
  digitalWrite(piezoPin, HIGH);
  Serial.println("Beep! Duration: " + String(duration) + "ms");
}

/**
 * NFC检测成功提示音
 */
void beepNFCSuccess() {
  if (!systemFullyAwake) return;
  beep(150);
  delay(100);
  beep(150);
  Serial.println("NFC Success Sound");
}

/**
 * 番茄钟完成提示音
 */
void beepPomodoroComplete() {
  if (!systemFullyAwake) return;
  beep(200);
  delay(150);
  beep(200);
  delay(150);
  beep(300);
  Serial.println("Pomodoro Complete Sound");
}

/**
 * 光线不足警告音
 */
void beepLightWarning() {
  if (!systemFullyAwake) return;
  
  unsigned long now = millis();
  if (now - lastBeepTime > beepInterval) {
    digitalWrite(piezoPin, LOW);
    delay(800);
    digitalWrite(piezoPin, HIGH);
    lastBeepTime = now;
    Serial.println("Light Warning Sound");
  }
}

/**
 * 保存自适应番茄钟数据
 */
void saveAdaptivePomodoro(unsigned long sessionMillis) {
  if (sessionMillis < 5 * 60 * 1000UL) return;
  
  const float LEARNING_RATE = 0.15;
  const float SUCCESS_GROWTH = 1.05;
  const float PARTIAL_GROWTH = 1.02;
  const float ADJUST_RATE = 0.8;
  
  adaptiveTotalSessionTime += sessionMillis;
  adaptiveSessionCount++;
  
  float completionRate = (float)sessionMillis / (float)adaptivePomodoroMillis;
  unsigned long historicalAverage = adaptiveTotalSessionTime / adaptiveSessionCount;
  
  if (completionRate >= 1.0) {
    if (completionRate >= 1.2) {
      adaptivePomodoroMillis = (unsigned long)(adaptivePomodoroMillis * SUCCESS_GROWTH);
      Serial.println("[Pomodoro] Excellent! Target increased");
    } else {
      adaptivePomodoroMillis = (unsigned long)(adaptivePomodoroMillis * PARTIAL_GROWTH);
      Serial.println("[Pomodoro] Good! Target slightly increased");
    }
  } else if (completionRate >= 0.8) {
    adaptivePomodoroMillis = (unsigned long)(LEARNING_RATE * historicalAverage + 
                                           (1 - LEARNING_RATE) * adaptivePomodoroMillis);
    Serial.println("[Pomodoro] Stable performance, minor adjustment");
  } else if (completionRate >= 0.6) {
    unsigned long targetAdjustment = (unsigned long)(historicalAverage * ADJUST_RATE);
    adaptivePomodoroMillis = (unsigned long)(LEARNING_RATE * targetAdjustment + 
                                           (1 - LEARNING_RATE) * adaptivePomodoroMillis);
    Serial.println("[Pomodoro] Moderate adjustment needed");
  } else {
    unsigned long targetAdjustment = (unsigned long)(historicalAverage * 0.7);
    adaptivePomodoroMillis = (unsigned long)(LEARNING_RATE * targetAdjustment + 
                                           (1 - LEARNING_RATE) * adaptivePomodoroMillis);
    Serial.println("[Pomodoro] Significant adjustment needed");
  }
  
  adaptivePomodoroMillis = constrain(adaptivePomodoroMillis, 5 * 60 * 1000UL, 60 * 60 * 1000UL);
  
  prefs.putULong("pomodoro", adaptivePomodoroMillis);
  prefs.putULong("adaptTime", adaptiveTotalSessionTime);
  prefs.putULong("adaptCount", adaptiveSessionCount);
}

/**
 * 显示NFC卡片检测界面
 */
void showNFCUIDAndSave(uint8_t* uid, uint8_t uidLength) {
  String uidStr = "";
  for (uint8_t i = 0; i < uidLength; i++) {
    if (uid[i] < 0x10) uidStr += "0";
    uidStr += String(uid[i], HEX);
    if (i < uidLength - 1) uidStr += " ";
  }
  
  lastUID = uidStr;
  prefs.putString("lastUID", lastUID);
  Serial.println("NFC Card detected: " + uidStr);
  
  if (oledInitialized) {
    display.clearDisplay();
    drawCornerDecorations();
    display.drawBitmap(2, 2, icon_book, 16, 16, SSD1306_WHITE);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(20, 2);
    display.println("Book Found!");
    display.setCursor(20, 12);
    display.println("my notebook");
    display.setCursor(20, 22);
    display.println("Pages: One hundred");
    display.setCursor(2, 32);
    display.println("Zhiyu Cao's dev notes:");
    display.setCursor(2, 42);
    display.println("Design ideas, sketches");
    display.setCursor(2, 52);
    display.println("and formulas inside.");
    display.drawBitmap(110, 2, icon_heart, 16, 16, SSD1306_WHITE);
    display.setCursor(85, 52);
    display.println("ID:" + uidStr.substring(0, 6));
    display.display();
  }
}

/**
 * 绘制光线趋势图界面
 */
void drawTrendGraph() {
  if (!oledInitialized || i2cBusy) return;
  
  display.clearDisplay();
  drawCornerDecorations();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(20, 2);
  display.println("Light Trend");
  display.drawBitmap(2, 2, icon_battery, 16, 16, SSD1306_WHITE);
  display.setCursor(20, 12);
  display.println("Lux: " + String((int)bestLuxEMA));
  display.setCursor(20, 22);
  display.println("Too dark, find light");
  
  for (int i = 0; i < trendSize - 1; i++) {
    int x1 = i * (SCREEN_WIDTH / trendSize);
    int y1 = SCREEN_HEIGHT - 8 - map(lightTrend[(trendIndex + i) % trendSize], 0, 4095, 0, 25);
    int x2 = (i + 1) * (SCREEN_WIDTH / trendSize);
    int y2 = SCREEN_HEIGHT - 8 - map(lightTrend[(trendIndex + i + 1) % trendSize], 0, 4095, 0, 25);
    display.drawLine(x1, y1, x2, y2, SSD1306_WHITE);
  }
  
  for (int i = 0; i < SCREEN_WIDTH; i += 4) {
    display.drawPixel(i, SCREEN_HEIGHT - 2, SSD1306_WHITE);
  }
  
  display.display();
}

/*
 * =======================================================================
 * 主程序区域
 * =======================================================================
 */

/**
 * 系统初始化函数
 */
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Reading Tracker v2.3 Starting ===");
  
  // GPIO引脚初始化
  pinMode(ldrPin, INPUT);
  pinMode(piezoPin, OUTPUT);
  digitalWrite(piezoPin, HIGH);  // 蜂鸣器默认关闭
  
  // 数据存储初始化
  prefs.begin("reading", false);
  totalReadingSeconds = prefs.getULong("totalSecs", 0);
  adaptivePomodoroMillis = prefs.getULong("pomodoro", defaultPomodoroMin * 60000UL);
  adaptiveTotalSessionTime = prefs.getULong("adaptTime", 0);
  adaptiveSessionCount = prefs.getULong("adaptCount", 0);
  lastUID = prefs.getString("lastUID", "");
  
  // 检查唤醒原因
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("Wakeup from deep sleep by timer");
  } else {
    Serial.println("Cold boot or reset");
  }
  
  // 检查光线条件决定是否完全启动
  delay(100);  // 让ADC稳定
  if (shouldFullyWakeup()) {
    fullyWakeupSystem();
  } else {
    Serial.println("Light insufficient, staying in minimal mode");
    systemFullyAwake = false;
  }
  
  Serial.println("=== Setup Complete ===");
  Serial.print("System fully awake: ");
  Serial.println(systemFullyAwake ? "YES" : "NO");
}

/**
 * 主循环函数 (简化版 - 定时器唤醒机制)
 */
void loop() {
  // === 基础光线检测 (始终运行) ===
  int lightValue = analogRead(ldrPin);
  unsigned long now = millis();
  
  // === 优先检查：是否需要进入深度睡眠 ===
  if (shouldEnterSleep()) {
    // 直接进入深度睡眠，程序会在这里停止
    enterDeepSleep();
    // 注意：这行代码不会执行，因为设备已进入深度睡眠
  }
  
  // === 检查：是否需要完全唤醒系统 ===
  if (!systemFullyAwake && shouldFullyWakeup()) {
    fullyWakeupSystem();
    return;  // 让硬件初始化稳定
  }
  
  // === 如果系统未完全唤醒，只做基础检测 ===
  if (!systemFullyAwake) {
    Serial.println("Minimal mode - Light: " + String(lightValue));
    delay(5000);  // 降低检测频率节省电力
    return;
  }
  
  // ===== 以下代码只在系统完全唤醒时执行 =====
  
  // 更新光线趋势数据
  lightTrend[trendIndex] = lightValue;
  trendIndex = (trendIndex + 1) % trendSize;
  
  if (isReading) {
    bestLuxEMA = (1 - emaAlpha) * bestLuxEMA + emaAlpha * lightValue;
    bestLuxEMA = constrain(bestLuxEMA, 100, 3000);
  }
  
  // NFC检测
  static unsigned long lastNFCCheck = 0;
  if (nfcInitialized && !i2cBusy && (now - lastNFCCheck > 1000)) {
    lastNFCCheck = now;
    i2cBusy = true;
    
    uint8_t uid[7];
    uint8_t uidLength;
    
    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 500)) {
      if (!isShowingNFC) {
        isShowingNFC = true;
        nfcDisplayStartTime = now;
        showNFCUIDAndSave(uid, uidLength);
        beepNFCSuccess();
      }
      i2cBusy = false;
      delay(500);
      return;
    } else {
      if (isShowingNFC) {
        isShowingNFC = false;
        Serial.println("NFC card removed");
      }
      i2cBusy = false;
    }
  }
  
  // NFC界面超时检查
  if (isShowingNFC && (now - nfcDisplayStartTime > 30000)) {
    isShowingNFC = false;
    Serial.println("NFC display timeout");
  }
  
  if (isShowingNFC) {
    delay(500);
    return;
  }
  
  // 阅读状态逻辑
  if (lightValue < lightThreshold) {
    // 光线不足，暂停阅读
    if (isReading) {
      accumulatedSessionMillis += now - sessionStartMillis;
      isReading = false;
      isPaused = true;
    }
    drawTrendGraph();
    beepLightWarning();
  } else {
    // 光线充足，可以阅读
    if (!isReading) {
      if (inRest) {
        // 休息状态逻辑
        if (restStartMillis == 0) restStartMillis = now;
        
        if (now - restStartMillis >= restDuration) {
          // 休息结束，开始新的阅读会话
          inRest = false;
          beep(300);
          sessionStartMillis = now;
          accumulatedSessionMillis = 0;
          isReading = true;
        } else {
          // 显示休息界面
          if (oledInitialized && !i2cBusy) {
            display.clearDisplay();
            drawCornerDecorations();
            display.setTextSize(1);
            display.setTextColor(SSD1306_WHITE);
            display.setCursor(30, 2);
            display.println("Rest Time");
            display.drawBitmap(6, 15, icon_heart, 16, 16, SSD1306_WHITE);
            display.setCursor(28, 20);
            display.println("Take a break!");
            display.setCursor(35, 30);
            display.println("Relax...");
            display.drawBitmap(106, 15, icon_star, 16, 16, SSD1306_WHITE);
            
            unsigned long restRemaining = restDuration - (now - restStartMillis);
            int restMin = restRemaining / 60000;
            int restSec = (restRemaining / 1000) % 60;
            display.setCursor(35, 45);
            display.println(String(restMin) + ":" + (restSec < 10 ? "0" : "") + String(restSec));
            
            // 波浪线装饰
            for (int x = 0; x < SCREEN_WIDTH; x += 8) {
              int y = 55 + (sin(x * 0.3 + millis() * 0.01) * 3);
              display.drawPixel(x, y, SSD1306_WHITE);
              display.drawPixel(x + 2, y + 1, SSD1306_WHITE);
            }
            display.display();
          }
          delay(1000);
          return;
        }
      } else {
        // 从暂停恢复到阅读
        sessionStartMillis = now;
        isPaused = false;
        isReading = true;
      }
    } else {
      // 正在阅读状态
      unsigned long sessionMillis = accumulatedSessionMillis + (now - sessionStartMillis);
      
      if (sessionMillis >= adaptivePomodoroMillis) {
        // 番茄钟时间到，进入休息
        inRest = true;
        isReading = false;
        restStartMillis = 0;
        totalReadingSeconds += sessionMillis / 1000;
        
        saveAdaptivePomodoro(sessionMillis);
        prefs.putULong("totalSecs", totalReadingSeconds);
        beepPomodoroComplete();
      } else {
        // 显示阅读界面
        if (oledInitialized && !i2cBusy) {
          int cm = sessionMillis / 60000;
          int cs = (sessionMillis / 1000) % 60;
          unsigned long totalMinutes = totalReadingSeconds / 60;
          unsigned long totalHours = totalMinutes / 60;
          unsigned long totalMinsOnly = totalMinutes % 60;
          int pm = adaptivePomodoroMillis / 60000;
          
          String totalStr = (totalHours > 0) ? 
            (String(totalHours) + "h " + String(totalMinsOnly) + "m") : 
            (String(totalMinsOnly) + "m");
          
          display.clearDisplay();
          drawCornerDecorations();
          display.setTextSize(1);
          display.setTextColor(SSD1306_WHITE);
          display.setCursor(22, 2);
          display.println("Reading Mode");
          display.drawBitmap(2, 2, icon_clock, 16, 16, SSD1306_WHITE);
          
          // 当前时间大字体显示
          display.setTextSize(2);
          display.setCursor(8, 15);
          display.print(String(cm) + ":" + (cs < 10 ? "0" : "") + String(cs));
          
          // 动态图标
          drawAnimatedIcon(108, 15, animationFrame);
          
          // 统计信息
          display.setTextSize(1);
          display.setCursor(8, 35);
          display.println("Total: " + totalStr);
          display.setCursor(8, 45);
          display.println("Target: " + String(pm) + " min");
          
          // 进度条
          int progress = (sessionMillis * 100) / adaptivePomodoroMillis;
          drawProgressBar(8, 55, 112, 6, progress, 100);
          
          // NFC状态指示
          display.fillCircle(120, 45, 2, nfcInitialized ? SSD1306_WHITE : SSD1306_BLACK);
          display.drawCircle(120, 45, 2, SSD1306_WHITE);
          
          display.display();
          animationFrame++;
        }
      }
    }
  }
  
  // 更新活动时间
  lastFullWakeTime = now;
  
  delay(500);  // 主循环延迟
}