/*
 * =======================================================================
 * ESP32 WROOM32D NFC智能阅读追踪器 - 安全GPIO唤醒版本（优化版）
 * =======================================================================
 * 
 * 重要说明：
 * - 避免使用GPIO 0，因为它控制启动模式
 * - 使用GPIO 26作为光敏电阻唤醒引脚（RTC_GPIO7）
 * - GPIO 36保留作为ADC输入（只读，不能输出）
 * - 优化：启动保护时间从30秒缩减到3秒，提高响应速度
 * - 优化：睡眠检测从10次连续检测缩减到3次，加速睡眠模式进入
 * 
 * 硬件连接（从ESP32C3迁移到ESP32 WROOM32D）：
 * ESP32 WROOM32D:
 * ├── OLED SSD1306 (128x64)
 * │   ├── SDA → GPIO 21 (标准I2C)
 * │   ├── SCL → GPIO 22 (标准I2C)
 * │   ├── VCC → 3.3V
 * │   └── GND → GND
 * ├── PN532 NFC模块
 * │   ├── SDA → GPIO 21 (与OLED共享I2C总线)
 * │   ├── SCL → GPIO 22 (与OLED共享I2C总线)
 * │   ├── VCC → 3.3V
 * │   └── GND → GND
 * ├── 光敏电阻 (LDR) - 双输入设计
 * │   ├── 一端 → 3.3V
 * │   ├── 另一端 → GPIO 36 (ADC输入，精确测量)
 * │   ├── 另一端 → GPIO 26 (数字输入，唤醒用，RTC_GPIO7)
 * │   └── 两个10kΩ下拉电阻分别到GND
 * ├── 蜂鸣器模块
 * │   ├── VCC → 3.3V
 * │   ├── GND → GND
 * │   └── Signal → GPIO 5
 * └── 可选：手动唤醒按钮
 *     └── 按钮 → GPIO 2 (RTC_GPIO12) 与GND之间
 * 
 * =======================================================================
 */

#include <Arduino.h>
#include "driver/ledc.h"
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "esp_system.h"
#include "esp_sleep.h"
#include <Adafruit_PN532.h>
#include <math.h>

// ===== 硬件引脚定义 (ESP32 WROOM32D - 安全版本) =====
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

#define SDA_PIN 21             // ESP32标准I2C引脚
#define SCL_PIN 22             // ESP32标准I2C引脚

const int ldrPin = 36;         // 光敏电阻ADC输入 (精确测量，只读)
const int ldrWakeupPin = 26;   // 光敏电阻数字输入 (唤醒用，RTC_GPIO7)
const int buttonWakeupPin = 2; // 手动唤醒按钮 (RTC_GPIO12) - 可选
const int piezoPin = 5;        // 蜂鸣器引脚

// ===== 优化配置 =====
const int lightThreshold = 1000;         // 光线阈值，低于此值认为光线不足
const int bookmarkThreshold = 50;        // 书签检测阈值，用于睡眠判断
const unsigned long STARTUP_GRACE_PERIOD = 3000;  // 启动保护时间：3秒（原30秒）
const int SLEEP_CHECK_COUNT = 3;          // 睡眠检测次数：3次（原10次）
const unsigned long SLEEP_CHECK_INTERVAL = 1000; // 睡眠检测间隔：1秒（原2秒）

// ===== 唤醒原因枚举 =====
enum WakeupReason {
  WAKEUP_TIMER,    // 定时器唤醒
  WAKEUP_LIGHT,    // 光线变化唤醒
  WAKEUP_BUTTON,   // 按钮唤醒
  WAKEUP_UNKNOWN   // 未知原因（首次启动）
};

// ===== 硬件对象初始化 =====
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_PN532 nfc(SDA_PIN, SCL_PIN);

// ===== 状态变量 =====
String lastUID = "";                      // 上次检测到的NFC卡片UID
bool showLastUIDOnBoot = false;           // 是否在启动时显示上次的卡片
bool nfcInitialized = false;              // NFC模块是否初始化成功
bool oledInitialized = false;             // OLED屏幕是否初始化成功
bool i2cBusy = false;                     // I2C总线是否忙碌
bool isShowingNFC = false;                // 是否正在显示NFC信息
unsigned long nfcDisplayStartTime = 0;    // NFC显示开始时间

// ===== 阅读状态变量 =====
bool isReading = false;                   // 是否正在阅读
bool isPaused = false;                    // 是否暂停（因光线不足）
bool inRest = false;                      // 是否在休息状态
bool isSleeping = false;                  // 是否在睡眠状态

// ===== 时间记录变量 =====
unsigned long sessionStartMillis = 0;        // 本次阅读开始时间
unsigned long accumulatedSessionMillis = 0;  // 累计阅读时间
unsigned long restStartMillis = 0;           // 休息开始时间
unsigned long totalReadingSeconds = 0;       // 总阅读时间（秒）

// ===== 自适应番茄钟变量 =====
const int defaultPomodoroMin = 25;                           // 默认番茄钟时长（分钟）
unsigned long adaptivePomodoroMillis = defaultPomodoroMin * 60000UL; // 自适应番茄钟时长
unsigned long adaptiveTotalSessionTime = 0;                 // 历史总会话时间
unsigned long adaptiveSessionCount = 0;                     // 历史会话次数
const unsigned long restDuration = 5 * 60 * 1000UL;        // 休息时长：5分钟

// ===== 蜂鸣器控制 =====
unsigned long lastBeepTime = 0;              // 上次蜂鸣时间
const unsigned long beepInterval = 10000;    // 蜂鸣间隔：10秒

// ===== 数据存储 =====
Preferences prefs;

// ===== 光线趋势分析 =====
const int trendSize = 30;        // 趋势数据点数量
int lightTrend[trendSize] = {0}; // 光线趋势数组
int trendIndex = 0;              // 当前趋势索引
float bestLuxEMA = 0.0;          // 最佳光照指数移动平均值
const float emaAlpha = 0.05;     // EMA平滑系数
int animationFrame = 0;          // 动画帧计数

// ===== 图标数据 =====
// 书本图标（16x16像素）
const unsigned char icon_book[] PROGMEM = {
  0x00, 0x00, 0x1F, 0xF8, 0x10, 0x08, 0x10, 0x08, 0x10, 0x08, 0x10, 0x08, 
  0x1F, 0xF8, 0x10, 0x08, 0x10, 0x08, 0x10, 0x08, 0x10, 0x08, 0x10, 0x08, 
  0x10, 0x08, 0x1F, 0xF8, 0x00, 0x00, 0x00, 0x00
};

// 时钟图标（16x16像素）
const unsigned char icon_clock[] PROGMEM = {
  0x03, 0xC0, 0x0C, 0x30, 0x10, 0x08, 0x20, 0x04, 0x20, 0x04, 0x41, 0x82, 
  0x41, 0x82, 0x41, 0x02, 0x41, 0x02, 0x20, 0x04, 0x20, 0x04, 0x10, 0x08, 
  0x0C, 0x30, 0x03, 0xC0, 0x00, 0x00, 0x00, 0x00
};

// 心形图标（16x16像素）
const unsigned char icon_heart[] PROGMEM = {
  0x00, 0x00, 0x0E, 0x70, 0x1F, 0xF8, 0x3F, 0xFC, 0x7F, 0xFE, 0x7F, 0xFE, 
  0x3F, 0xFC, 0x1F, 0xF8, 0x0F, 0xF0, 0x07, 0xE0, 0x03, 0xC0, 0x01, 0x80, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// 星星图标（16x16像素）
const unsigned char icon_star[] PROGMEM = {
  0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x0D, 0xB0, 0x1F, 0xF8, 0x0F, 0xF0, 
  0x07, 0xE0, 0x0F, 0xF0, 0x1B, 0xD8, 0x31, 0x8C, 0x60, 0x06, 0x60, 0x06, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// 电池图标（16x16像素）
const unsigned char icon_battery[] PROGMEM = {
  0x00, 0x00, 0x3F, 0xF0, 0x20, 0x18, 0x20, 0x18, 0x27, 0x98, 0x27, 0x98, 
  0x27, 0x98, 0x27, 0x98, 0x27, 0x98, 0x27, 0x98, 0x20, 0x18, 0x20, 0x18, 
  0x3F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// ===== 函数声明 =====
void beep(int duration = 100);                              // 基础蜂鸣函数
void beepNFCSuccess();                                       // NFC检测成功提示音
void beepPomodoroComplete();                                 // 番茄钟完成提示音
void beepLightWarning();                                     // 光线警告提示音
void saveAdaptivePomodoro(unsigned long sessionMillis);     // 保存自适应番茄钟设置
void showNFCUIDAndSave(uint8_t* uid, uint8_t uidLength);    // 显示并保存NFC信息
void drawTrendGraph();                                       // 绘制光线趋势图
void drawAnimatedIcon(int x, int y, int frame);              // 绘制动画图标
void drawProgressBar(int x, int y, int width, int height, int progress, int maxProgress); // 绘制进度条
void drawCornerDecorations();                                // 绘制屏幕装饰边框
bool initializeOLED();                                       // 初始化OLED显示屏
bool initializeNFC();                                        // 初始化NFC模块
void turnOffOLED();                                          // 关闭OLED显示屏
void turnOnOLED();                                           // 开启OLED显示屏
void scanI2CDevices();                                       // 扫描I2C设备
WakeupReason getWakeupReason();                              // 获取唤醒原因
void configureWakeupSources();                               // 配置唤醒源
void enterSmartSleep();                                      // 进入智能睡眠模式
void wakeupInitialization();                                 // 唤醒后初始化
bool shouldEnterSleep();                                     // 判断是否应该进入睡眠

/**
 * 检测唤醒原因
 * 分析ESP32从深度睡眠中唤醒的具体原因
 */
WakeupReason getWakeupReason() {
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  
  switch(wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0:  // GPIO 26 (光敏电阻) 唤醒
      Serial.println("Wakeup caused by GPIO 26 (Light sensor)");
      return WAKEUP_LIGHT;
      
    case ESP_SLEEP_WAKEUP_EXT1: { // 多GPIO唤醒（备用）
      Serial.println("Wakeup caused by EXT1 (multiple GPIO)");
      uint64_t wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();
      if (wakeup_pin_mask & (1ULL << 26)) {
        return WAKEUP_LIGHT;
      }
      if (wakeup_pin_mask & (1ULL << 2)) {
        return WAKEUP_BUTTON;
      }
      return WAKEUP_UNKNOWN;
    }
      
    case ESP_SLEEP_WAKEUP_TIMER:  // 定时器唤醒（安全网）
      Serial.println("Wakeup caused by timer");
      return WAKEUP_TIMER;
      
    default:  // 首次启动或重启
      Serial.println("Wakeup was not caused by deep sleep (first boot or reset)");
      return WAKEUP_UNKNOWN;
  }
}

/**
 * 配置外部唤醒源
 * 设置GPIO和定时器作为唤醒触发条件
 */
void configureWakeupSources() {
  // 使用EXT0唤醒 (单GPIO)
  // GPIO 26 (光敏电阻): 高电平唤醒 (光线充足时)
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_26, 1);  // 1 = 高电平唤醒
  
  // 备用：设置一个较长的定时器唤醒（30分钟）作为安全网
  // 防止系统长时间无法唤醒
  esp_sleep_enable_timer_wakeup(30 * 60 * 1000000ULL); // 30分钟
  
  Serial.println("Safe wakeup sources configured:");
  Serial.println("- GPIO 26 (LDR): HIGH level (bright light)");
  Serial.println("- Timer: 30 minutes (safety)");
  Serial.println("- GPIO 0 avoided (safe for boot process)");
}

/**
 * 智能睡眠函数
 * 保存当前状态并进入深度睡眠模式
 */
void enterSmartSleep() {
  Serial.println("=== Entering Smart Sleep Mode ===");
  
  // 保存当前阅读状态和数据
  if (isReading || isPaused) {
    unsigned long now = millis();
    unsigned long sessionMillis = accumulatedSessionMillis;
    if (isReading) sessionMillis += now - sessionStartMillis;
    totalReadingSeconds += sessionMillis / 1000;
    
    // 保存自适应番茄钟数据
    saveAdaptivePomodoro(sessionMillis);
    prefs.putULong("totalSecs", totalReadingSeconds);
  }
  
  // 显示睡眠界面信息
  if (oledInitialized && !i2cBusy) {
    display.clearDisplay();
    drawCornerDecorations();
    
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(25, 2);
    display.println("Smart Sleep");
    
    display.drawBitmap(6, 20, icon_battery, 16, 16, SSD1306_WHITE);
    
    display.setCursor(28, 20);
    display.println("Will wake on:");
    display.setCursor(28, 30);
    display.println("* Light change");
    display.setCursor(28, 40);
    display.println("* Button press");
    
    display.drawBitmap(106, 20, icon_clock, 16, 16, SSD1306_WHITE);
    
    display.display();
    delay(2000);  // 给用户2秒时间阅读信息
  }
  
  // 关闭外设以节省电力
  turnOffOLED();
  
  // 配置唤醒源
  configureWakeupSources();
  
  // 进入深度睡眠
  Serial.println("Going to sleep now...");
  esp_deep_sleep_start();
}

/**
 * 光线检测逻辑 - 优化版本
 * 快速检测是否应该进入睡眠模式，减少检测次数提高响应速度
 */
bool shouldEnterSleep() {
  static bool sleepCheckEnabled = false;
  static unsigned long systemStartTime = millis();
  static int lowLightCount = 0;        // 低光照计数器
  static unsigned long lastCheck = 0;
  
  unsigned long now = millis();
  int lightValue = analogRead(ldrPin);
  
  // 优化：系统启动后3秒内不检查睡眠（原30秒）
  if (now - systemStartTime < STARTUP_GRACE_PERIOD) {
    Serial.println("System startup grace period, sleep check disabled");
    return false;
  }
  
  // 如果正在显示NFC或处于活跃状态，不进入睡眠
  if (isShowingNFC || isReading) {
    lowLightCount = 0;
    return false;
  }
  
  // 优化：每1秒检查一次（原2秒），提高响应速度
  if (now - lastCheck > SLEEP_CHECK_INTERVAL) {
    lastCheck = now;
    
    Serial.println("Light check - Value: " + String(lightValue) + ", Threshold: " + String(bookmarkThreshold));
    
    if (lightValue < bookmarkThreshold) {
      lowLightCount++;
      Serial.println("Low light detected, count: " + String(lowLightCount) + "/" + String(SLEEP_CHECK_COUNT));
    } else {
      lowLightCount = 0;
      Serial.println("Light sufficient, count reset");
    }
  }
  
  // 优化：只需要连续3次检查（3秒）都是低光照就进入睡眠（原10次20秒）
  if (lowLightCount >= SLEEP_CHECK_COUNT) {
    Serial.println("Entering sleep mode due to prolonged low light");
    return true;
  }
  
  return false;
}

/**
 * 从睡眠中恢复的初始化
 * 重新启动硬件并显示欢迎信息
 */
void wakeupInitialization() {
  WakeupReason reason = getWakeupReason();
  
  // 重新初始化硬件
  turnOnOLED();
  
  if (oledInitialized) {
    display.clearDisplay();
    drawCornerDecorations();
    
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(30, 2);
    display.println("Welcome Back!");
    
    // 根据不同的唤醒原因显示相应信息
    switch(reason) {
      case WAKEUP_LIGHT:
        display.drawBitmap(6, 20, icon_book, 16, 16, SSD1306_WHITE);
        display.setCursor(28, 20);
        display.println("Light detected");
        display.setCursor(28, 30);
        display.println("Ready to read!");
        break;
        
      case WAKEUP_BUTTON:
        display.drawBitmap(6, 20, icon_heart, 16, 16, SSD1306_WHITE);
        display.setCursor(28, 20);
        display.println("Button pressed");
        display.setCursor(28, 30);
        display.println("Manual wakeup");
        break;
        
      case WAKEUP_TIMER:
        display.drawBitmap(6, 20, icon_clock, 16, 16, SSD1306_WHITE);
        display.setCursor(28, 20);
        display.println("Timer wakeup");
        display.setCursor(28, 30);
        display.println("Safety check");
        break;
        
      default:
        display.drawBitmap(6, 20, icon_star, 16, 16, SSD1306_WHITE);
        display.setCursor(28, 20);
        display.println("System start");
        break;
    }
    
    display.display();
    delay(2000);
  }
  
  // 播放唤醒提示音
  beep(200);
  delay(100);
  beep(200);
}

/**
 * I2C设备扫描函数
 * 扫描并显示所有连接的I2C设备地址
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
 * 增加界面美观性
 */
void drawCornerDecorations() {
  // 左上角
  display.drawLine(0, 0, 8, 0, SSD1306_WHITE);
  display.drawLine(0, 0, 0, 8, SSD1306_WHITE);
  // 右上角
  display.drawLine(119, 0, 127, 0, SSD1306_WHITE);
  display.drawLine(127, 0, 127, 8, SSD1306_WHITE);
  // 左下角
  display.drawLine(0, 56, 8, 56, SSD1306_WHITE);
  display.drawLine(0, 56, 0, 63, SSD1306_WHITE);
  // 右下角
  display.drawLine(119, 63, 127, 63, SSD1306_WHITE);
  display.drawLine(127, 55, 127, 63, SSD1306_WHITE);
}

/**
 * 绘制动态轮播图标
 * 循环显示不同图标实现动画效果
 */
void drawAnimatedIcon(int x, int y, int frame) {
  const unsigned char* icons[] = {icon_book, icon_heart, icon_star, icon_clock};
  int iconIndex = (frame / 20) % 4;  // 每20帧切换一次图标
  display.drawBitmap(x, y, icons[iconIndex], 16, 16, SSD1306_WHITE);
}

/**
 * 绘制进度条
 * 显示阅读进度或休息进度
 */
void drawProgressBar(int x, int y, int width, int height, int progress, int maxProgress) {
  // 绘制外框
  display.drawRect(x, y, width, height, SSD1306_WHITE);
  // 计算填充宽度
  int fillWidth = (progress * (width - 2)) / maxProgress;
  if (fillWidth > 0) {
    // 填充进度
    display.fillRect(x + 1, y + 1, fillWidth, height - 2, SSD1306_WHITE);
  }
}

/**
 * 蜂鸣器基础函数
 * 产生指定时长的蜂鸣声
 */
void beep(int duration) {
  digitalWrite(piezoPin, LOW);   // 开启蜂鸣器
  delay(duration);
  digitalWrite(piezoPin, HIGH);  // 关闭蜂鸣器
  Serial.println("Beep! Duration: " + String(duration) + "ms");
}

/**
 * NFC检测成功提示音
 * 双声调表示成功检测到NFC卡片
 */
void beepNFCSuccess() {
  beep(150);
  delay(100);
  beep(150);
  Serial.println("NFC Success Sound");
}

/**
 * 番茄钟完成提示音
 * 三声长音表示完成一个番茄钟周期
 */
void beepPomodoroComplete() {
  beep(200);
  delay(150);
  beep(200);
  delay(150);
  beep(300);
  Serial.println("Pomodoro Complete Sound");
}

/**
 * 光线警告提示音
 * 长音警告光线不足，有防重复机制
 */
void beepLightWarning() {
  unsigned long now = millis();
  if (now - lastBeepTime > beepInterval) {  // 防止频繁蜂鸣
    digitalWrite(piezoPin, LOW);
    delay(800);  // 长音800毫秒
    digitalWrite(piezoPin, HIGH);
    lastBeepTime = now;
    Serial.println("Light Warning Sound");
  }
}

/**
 * 自适应番茄钟保存函数
 * 根据用户实际阅读表现调整番茄钟时长
 * 使用机器学习算法动态优化时间设置
 */
void saveAdaptivePomodoro(unsigned long sessionMillis) {
  // 过滤过短的会话（少于5分钟）
  if (sessionMillis < 5 * 60 * 1000UL) return;
  
  // 学习率和增长率常数
  const float LEARNING_RATE = 0.15;      // 学习率
  const float SUCCESS_GROWTH = 1.05;     // 超额完成增长率
  const float PARTIAL_GROWTH = 1.02;     // 部分完成增长率
  const float ADJUST_RATE = 0.8;         // 调整率
  
  // 更新历史数据
  adaptiveTotalSessionTime += sessionMillis;
  adaptiveSessionCount++;
  
  // 计算完成率和历史平均值
  float completionRate = (float)sessionMillis / (float)adaptivePomodoroMillis;
  unsigned long historicalAverage = adaptiveTotalSessionTime / adaptiveSessionCount;
  
  // 根据完成率调整番茄钟时长
  if (completionRate >= 1.0) {
    // 完成或超额完成
    if (completionRate >= 1.2) {
      adaptivePomodoroMillis = (unsigned long)(adaptivePomodoroMillis * SUCCESS_GROWTH);
      Serial.println("[Pomodoro] Excellent! Target increased");
    } else {
      adaptivePomodoroMillis = (unsigned long)(adaptivePomodoroMillis * PARTIAL_GROWTH);
      Serial.println("[Pomodoro] Good! Target slightly increased");
    }
  } else if (completionRate >= 0.8) {
    // 80%-100%完成率：微调
    adaptivePomodoroMillis = (unsigned long)(LEARNING_RATE * historicalAverage + 
                                           (1 - LEARNING_RATE) * adaptivePomodoroMillis);
    Serial.println("[Pomodoro] Stable performance, minor adjustment");
  } else if (completionRate >= 0.6) {
    // 60%-80%完成率：适度调整
    unsigned long targetAdjustment = (unsigned long)(historicalAverage * ADJUST_RATE);
    adaptivePomodoroMillis = (unsigned long)(LEARNING_RATE * targetAdjustment + 
                                           (1 - LEARNING_RATE) * adaptivePomodoroMillis);
    Serial.println("[Pomodoro] Moderate adjustment needed");
  } else {
    // 低于60%完成率：显著调整
    unsigned long targetAdjustment = (unsigned long)(historicalAverage * 0.7);
    adaptivePomodoroMillis = (unsigned long)(LEARNING_RATE * targetAdjustment + 
                                           (1 - LEARNING_RATE) * adaptivePomodoroMillis);
    Serial.println("[Pomodoro] Significant adjustment needed");
  }
  
  // 限制番茄钟时长范围（5分钟-60分钟）
  adaptivePomodoroMillis = constrain(adaptivePomodoroMillis, 
                                   5 * 60 * 1000UL, 
                                   60 * 60 * 1000UL);
  
  // 保存到Flash存储
  prefs.putULong("pomodoro", adaptivePomodoroMillis);
  prefs.putULong("adaptTime", adaptiveTotalSessionTime);
  prefs.putULong("adaptCount", adaptiveSessionCount);
  
  // 输出调整信息
  Serial.println("=== 自适应番茄钟调整 ===");
  Serial.println("完成率: " + String(completionRate * 100, 1) + "%");
  Serial.println("新目标: " + String(adaptivePomodoroMillis / 60000) + "分钟");
  Serial.println("==================");
}

/**
 * 显示NFC检测界面
 * 当检测到NFC卡片时显示书籍信息
 */
void showNFCUIDAndSave(uint8_t* uid, uint8_t uidLength) {
  // 将UID转换为十六进制字符串
  String uidStr = "";
  for (uint8_t i = 0; i < uidLength; i++) {
    if (uid[i] < 0x10) uidStr += "0";
    uidStr += String(uid[i], HEX);
    if (i < uidLength - 1) uidStr += " ";
  }
  
  // 保存UID信息
  lastUID = uidStr;
  prefs.putString("lastUID", lastUID);
  
  Serial.println("NFC Card detected: " + uidStr);
  
  delay(100);
  
  // 显示书籍信息界面
  if (oledInitialized) {
    display.clearDisplay();
    drawCornerDecorations();
    
    display.drawBitmap(2, 2, icon_book, 16, 16, SSD1306_WHITE);
    
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(20, 2);
    display.println("Book Found!");
    
    display.setCursor(20, 12);
    display.println("my notebook");  // 书名
    
    display.setCursor(20, 22);
    display.println("Pages: One hundred");  // 页数信息
    
    // 书籍描述
    display.setCursor(2, 32);
    display.println("Zhiyu Cao's dev notes:");
    display.setCursor(2, 42);
    display.println("Design ideas, sketches");
    display.setCursor(2, 52);
    display.println("and formulas inside.");
    
    display.drawBitmap(110, 2, icon_heart, 16, 16, SSD1306_WHITE);
    
    // 显示UID前6位
    display.setCursor(85, 52);
    display.println("ID:" + uidStr.substring(0, 6));
    
    display.display();
  }
  
  Serial.println("NFC card detected with beep sound");
}

/**
 * 绘制光线趋势图
 * 当光线不足时显示光线变化趋势和警告
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
  
  // 显示光照数值
  display.setCursor(20, 12);
  display.println("Lux: " + String((int)bestLuxEMA));
  
  display.setCursor(20, 22);
  display.println("Too dark, find the light");
  
  // 绘制光线趋势曲线
  for (int i = 0; i < trendSize - 1; i++) {
    int x1 = i * (SCREEN_WIDTH / trendSize);
    int y1 = SCREEN_HEIGHT - 8 - map(lightTrend[(trendIndex + i) % trendSize], 0, 4095, 0, 25);
    int x2 = (i + 1) * (SCREEN_WIDTH / trendSize);
    int y2 = SCREEN_HEIGHT - 8 - map(lightTrend[(trendIndex + i + 1) % trendSize], 0, 4095, 0, 25);
    
    display.drawLine(x1, y1, x2, y2, SSD1306_WHITE);
  }
  
  // 底部装饰点线
  for (int i = 0; i < SCREEN_WIDTH; i += 4) {
    display.drawPixel(i, SCREEN_HEIGHT - 2, SSD1306_WHITE);
  }
  
  display.display();
}

/**
 * 初始化OLED显示屏
 * 配置显示参数并显示启动动画
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
  
  // 显示功能图标
  display.drawBitmap(30, 20, icon_book, 16, 16, SSD1306_WHITE);
  display.drawBitmap(50, 20, icon_heart, 16, 16, SSD1306_WHITE);
  display.drawBitmap(70, 20, icon_clock, 16, 16, SSD1306_WHITE);
  
  display.setCursor(45, 45);
  display.println("v2.3-Fast");  // 版本号更新
  
  // 底部装饰线
  for (int i = 0; i < SCREEN_WIDTH; i += 8) {
    display.drawPixel(i, 55, SSD1306_WHITE);
  }
  
  display.display();
  delay(2000);
  
  // 显示启动状态
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
 * 配置PN532芯片并验证固件版本
 */
bool initializeNFC() {
  Serial.println("Initializing NFC...");
  nfc.begin();
  
  // 获取固件版本验证连接
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("PN532 not found!");
    return false;
  }
  
  Serial.print("Found PN532, FW ver: ");
  Serial.print((versiondata >> 16) & 0xFF, DEC);
  Serial.print('.');
  Serial.println((versiondata >> 8) & 0xFF, DEC);
  
  // 配置SAM (Security and Access Module)
  nfc.SAMConfig();
  Serial.println("NFC initialized successfully");
  return true;
}

/**
 * 关闭OLED显示屏
 * 清屏、关闭显示并释放I2C总线
 */
void turnOffOLED() {
  if (oledInitialized) {
    display.clearDisplay();
    display.display();
    display.ssd1306_command(SSD1306_DISPLAYOFF);  // 关闭显示
    delay(100);
  }
  Wire.end();  // 释放I2C总线
  
  // 设置引脚为上拉输入模式以节省电力
  pinMode(SDA_PIN, INPUT_PULLUP);
  pinMode(SCL_PIN, INPUT_PULLUP);
}

/**
 * 开启OLED显示屏
 * 重新初始化I2C总线和OLED显示
 */
void turnOnOLED() {
  Wire.begin(SDA_PIN, SCL_PIN);  // 重新初始化I2C
  delay(100);
  oledInitialized = initializeOLED();
}

/**
 * setup函数 - 系统初始化
 * 配置硬件、初始化外设、加载用户数据
 */
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Reading Tracker Starting (Optimized GPIO Wakeup Version) ===");
  
  // 检查唤醒原因并执行相应初始化
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED) {
    wakeupInitialization();
  }
  
  // GPIO初始化 (安全版本，避免使用GPIO 0)
  pinMode(ldrPin, INPUT);                    // GPIO 36: ADC输入 (只读，精确测量)
  pinMode(ldrWakeupPin, INPUT);              // GPIO 26: 数字输入 (唤醒用)
  pinMode(buttonWakeupPin, INPUT_PULLUP);    // GPIO 2: 按钮输入 (可选)
  pinMode(piezoPin, OUTPUT);                 // GPIO 5: 蜂鸣器输出
  digitalWrite(piezoPin, HIGH);              // 默认关闭蜂鸣器
  
  Serial.println("GPIO configuration:");
  Serial.println("- GPIO 36: LDR ADC input (precise measurement)");
  Serial.println("- GPIO 26: LDR digital input (wakeup trigger)");
  Serial.println("- GPIO 2: Button input (optional manual wakeup)");
  Serial.println("- GPIO 0: AVOIDED (safe for boot process)");
  
  // I2C总线初始化
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(500);
  
  // 扫描I2C设备
  scanI2CDevices();
  
  // 初始化OLED显示屏
  oledInitialized = initializeOLED();
  if (!oledInitialized) {
    Serial.println("OLED failed, continuing without display...");
  }
  
  // 初始化Flash存储
  prefs.begin("reading", false);
  
  // 从Flash加载用户数据
  totalReadingSeconds = prefs.getULong("totalSecs", 0);
  adaptivePomodoroMillis = prefs.getULong("pomodoro", defaultPomodoroMin * 60000UL);
  adaptiveTotalSessionTime = prefs.getULong("adaptTime", 0);
  adaptiveSessionCount = prefs.getULong("adaptCount", 0);
  
  // 初始化NFC模块
  nfcInitialized = initializeNFC();
  if (!nfcInitialized) {
    Serial.println("NFC failed, continuing without NFC...");
  }
  
  // 显示上次书籍信息（如果存在）
  lastUID = prefs.getString("lastUID", "");
  if (lastUID.length() > 0 && oledInitialized) {
    display.clearDisplay();
    drawCornerDecorations();
    
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(25, 2);
    display.println("Welcome Back!");
    
    display.drawBitmap(6, 15, icon_book, 16, 16, SSD1306_WHITE);
    
    display.setCursor(28, 15);
    display.println("Last Book:");
    display.setCursor(28, 25);
    display.println("my notebook");
    display.setCursor(28, 35);
    display.println(lastUID.substring(0, 8));
    
    display.drawBitmap(106, 15, icon_heart, 16, 16, SSD1306_WHITE);
    
    // 星级显示
    for (int i = 0; i < 3; i++) {
      display.drawBitmap(30 + i * 25, 50, icon_star, 16, 16, SSD1306_WHITE);
    }
    
    display.display();
    delay(3000);
  }
  
  // 输出初始化结果
  Serial.println("=== Setup Complete ===");
  Serial.print("OLED: ");
  Serial.println(oledInitialized ? "OK" : "Failed");
  Serial.print("NFC: ");
  Serial.println(nfcInitialized ? "OK" : "Failed");
  Serial.println("Optimizations applied:");
  Serial.println("- Startup grace period: 3 seconds");
  Serial.println("- Sleep check count: 3 times");
  Serial.println("- Check interval: 1 second");
  
  // 启动完成提示音
  beep(200);
  delay(100);
  beep(200);
  Serial.println("System ready with optimized GPIO wakeup support!");
}

/**
 * 主循环 - 优化版本
 * 处理光线检测、NFC读取、阅读状态管理和睡眠控制
 */
void loop() {
  // 读取光线传感器值并更新趋势数据
  int lightValue = analogRead(ldrPin);
  lightTrend[trendIndex] = lightValue;
  trendIndex = (trendIndex + 1) % trendSize;

  // 如果正在阅读，更新最佳光照EMA值
  if (isReading) {
    bestLuxEMA = (1 - emaAlpha) * bestLuxEMA + emaAlpha * lightValue;
    bestLuxEMA = constrain(bestLuxEMA, 100, 3000);
  }

  unsigned long now = millis();
  
  // 调试信息输出（每5秒一次）
  static unsigned long lastDebugPrint = 0;
  if (now - lastDebugPrint > 5000) {
    lastDebugPrint = now;
    Serial.println("=== System Status ===");
    Serial.println("Light Value: " + String(lightValue));
    Serial.println("Is Reading: " + String(isReading));
    Serial.println("Is Showing NFC: " + String(isShowingNFC));
    Serial.println("In Rest: " + String(inRest));
    Serial.println("==================");
  }
  
  // NFC检测逻辑（移到睡眠检测之前，确保优先级）
  static unsigned long lastNFCCheck = 0;
  if (nfcInitialized && !i2cBusy && (now - lastNFCCheck > 1000)) {
    lastNFCCheck = now;
    i2cBusy = true;
    
    uint8_t uid[7];
    uint8_t uidLength;
    
    // 尝试读取NFC卡片（500ms超时）
    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 500)) {
      if (!isShowingNFC) {
        isShowingNFC = true;
        nfcDisplayStartTime = now;
        showNFCUIDAndSave(uid, uidLength);
        beepNFCSuccess();
      }
      i2cBusy = false;
    } else {
      if (isShowingNFC) {
        isShowingNFC = false;
        Serial.println("NFC card removed, returning to normal mode");
      }
      i2cBusy = false;
    }
  }
  
  // NFC显示超时处理（30秒后自动返回）
  if (isShowingNFC) {
    if (now - nfcDisplayStartTime > 30000) {
      isShowingNFC = false;
      Serial.println("NFC display timeout, returning to normal mode");
    } else {
      delay(500);
      return;  // 在显示NFC信息期间跳过其他逻辑
    }
  }

  // 智能睡眠检测（优化版：只在非活跃状态下检查）
  if (!isReading && !inRest && !isShowingNFC && shouldEnterSleep()) {
    Serial.println("All conditions met for sleep, entering sleep mode...");
    enterSmartSleep();
  }
  
  // 光线阈值判断和阅读状态管理
  if (lightValue < lightThreshold) {
    // 光线不足时暂停阅读
    if (isReading) {
      accumulatedSessionMillis += now - sessionStartMillis;
      isReading = false;
      isPaused = true;
      Serial.println("Reading paused due to low light");
    }
    
    // 显示光线趋势图和警告（不在显示NFC时）
    if (!isShowingNFC) {
      drawTrendGraph();
      beepLightWarning();
    }
  } else {
    // 光线充足时开始或继续阅读
    if (!isReading && !inRest) {
      isReading = true;
      isPaused = false;
      sessionStartMillis = now;
      Serial.println("Reading session started");
    }
    
    // 阅读模式显示界面
    if (isReading && oledInitialized && !i2cBusy && !isShowingNFC) {
      unsigned long currentSessionMillis = now - sessionStartMillis + accumulatedSessionMillis;
      unsigned long targetMillis = adaptivePomodoroMillis;
      
      display.clearDisplay();
      drawCornerDecorations();
      
      // 显示阅读模式信息
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(25, 2);
      display.println("Reading Mode");
      
      display.drawBitmap(6, 15, icon_book, 16, 16, SSD1306_WHITE);
      
      display.setCursor(28, 15);
      display.println("Session: " + String(currentSessionMillis / 60000) + "m");
      display.setCursor(28, 25);
      display.println("Target: " + String(targetMillis / 60000) + "m");
      display.setCursor(28, 35);
      display.println("Light: " + String(lightValue));
      
      // 显示进度条
      int progress = (currentSessionMillis * 100) / targetMillis;
      progress = constrain(progress, 0, 100);
      drawProgressBar(6, 50, 115, 8, progress, 100);
      
      display.display();
      
      // 检查番茄钟是否完成
      if (currentSessionMillis >= targetMillis) {
        beepPomodoroComplete();
        saveAdaptivePomodoro(currentSessionMillis);
        
        // 进入休息模式
        isReading = false;
        inRest = true;
        restStartMillis = now;
        accumulatedSessionMillis = 0;
        
        Serial.println("Pomodoro completed! Entering rest mode.");
      }
    }
  }
  
  // 休息模式处理
  if (inRest && oledInitialized && !i2cBusy && !isShowingNFC) {
    unsigned long restElapsed = now - restStartMillis;
    unsigned long restRemaining = restDuration - restElapsed;
    
    if (restElapsed >= restDuration) {
      inRest = false;
      Serial.println("Rest period completed");
    } else {
      display.clearDisplay();
      drawCornerDecorations();
      
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(30, 2);
      display.println("Rest Time");
      
      display.drawBitmap(6, 20, icon_heart, 16, 16, SSD1306_WHITE);
      
      display.setCursor(28, 20);
      display.println("Take a break!");
      display.setCursor(28, 30);
      display.println("Remaining: " + String(restRemaining / 60000) + "m");
      
      // 休息进度条
      int restProgress = (restElapsed * 100) / restDuration;
      drawProgressBar(6, 50, 115, 8, restProgress, 100);
      
      display.display();
    }
  }
  
  delay(500);  // 主循环延迟，平衡响应速度和功耗
}