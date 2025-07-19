/*
 * =======================================================================
 * ESP32 WROOM32D NFC智能阅读追踪器 - 安全GPIO唤醒版本
 * =======================================================================
 * 
 * 重要说明：
 * - 避免使用GPIO 0，因为它控制启动模式
 * - 使用GPIO 26作为光敏电阻唤醒引脚（RTC_GPIO7）
 * - GPIO 36保留作为ADC输入（只读，不能输出）
 * - 这样既安全又可靠
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

// ===== 新增：唤醒配置 =====
const int lightThreshold = 1000;
const int bookmarkThreshold = 50;

// ===== 唤醒原因枚举 =====
enum WakeupReason {
  WAKEUP_TIMER,
  WAKEUP_LIGHT,
  WAKEUP_BUTTON,
  WAKEUP_UNKNOWN
};

// ===== 原有变量保持不变 =====
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_PN532 nfc(SDA_PIN, SCL_PIN);

String lastUID = "";
bool showLastUIDOnBoot = false;
bool nfcInitialized = false;
bool oledInitialized = false;
bool i2cBusy = false;
bool isShowingNFC = false;
unsigned long nfcDisplayStartTime = 0;

// 阅读状态变量
bool isReading = false;
bool isPaused = false;
bool inRest = false;
bool isSleeping = false;

// 时间记录变量
unsigned long sessionStartMillis = 0;
unsigned long accumulatedSessionMillis = 0;
unsigned long restStartMillis = 0;
unsigned long totalReadingSeconds = 0;

// 番茄钟变量
const int defaultPomodoroMin = 25;
unsigned long adaptivePomodoroMillis = defaultPomodoroMin * 60000UL;
unsigned long adaptiveTotalSessionTime = 0;
unsigned long adaptiveSessionCount = 0;
const unsigned long restDuration = 5 * 60 * 1000UL;

// 蜂鸣器控制
unsigned long lastBeepTime = 0;
const unsigned long beepInterval = 10000;

// 数据存储
Preferences prefs;

// 光线趋势分析
const int trendSize = 30;
int lightTrend[trendSize] = {0};
int trendIndex = 0;
float bestLuxEMA = 0.0;
const float emaAlpha = 0.05;
int animationFrame = 0;

// ===== 图标数据 =====
const unsigned char icon_book[] PROGMEM = {
  0x00, 0x00, 0x1F, 0xF8, 0x10, 0x08, 0x10, 0x08, 0x10, 0x08, 0x10, 0x08, 
  0x1F, 0xF8, 0x10, 0x08, 0x10, 0x08, 0x10, 0x08, 0x10, 0x08, 0x10, 0x08, 
  0x10, 0x08, 0x1F, 0xF8, 0x00, 0x00, 0x00, 0x00
};

const unsigned char icon_clock[] PROGMEM = {
  0x03, 0xC0, 0x0C, 0x30, 0x10, 0x08, 0x20, 0x04, 0x20, 0x04, 0x41, 0x82, 
  0x41, 0x82, 0x41, 0x02, 0x41, 0x02, 0x20, 0x04, 0x20, 0x04, 0x10, 0x08, 
  0x0C, 0x30, 0x03, 0xC0, 0x00, 0x00, 0x00, 0x00
};

const unsigned char icon_heart[] PROGMEM = {
  0x00, 0x00, 0x0E, 0x70, 0x1F, 0xF8, 0x3F, 0xFC, 0x7F, 0xFE, 0x7F, 0xFE, 
  0x3F, 0xFC, 0x1F, 0xF8, 0x0F, 0xF0, 0x07, 0xE0, 0x03, 0xC0, 0x01, 0x80, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

const unsigned char icon_star[] PROGMEM = {
  0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x0D, 0xB0, 0x1F, 0xF8, 0x0F, 0xF0, 
  0x07, 0xE0, 0x0F, 0xF0, 0x1B, 0xD8, 0x31, 0x8C, 0x60, 0x06, 0x60, 0x06, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

const unsigned char icon_battery[] PROGMEM = {
  0x00, 0x00, 0x3F, 0xF0, 0x20, 0x18, 0x20, 0x18, 0x27, 0x98, 0x27, 0x98, 
  0x27, 0x98, 0x27, 0x98, 0x27, 0x98, 0x27, 0x98, 0x20, 0x18, 0x20, 0x18, 
  0x3F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// ===== 函数声明 =====
void beep(int duration = 100);
void beepNFCSuccess();
void beepPomodoroComplete();
void beepLightWarning();
void saveAdaptivePomodoro(unsigned long sessionMillis);
void showNFCUIDAndSave(uint8_t* uid, uint8_t uidLength);
void drawTrendGraph();
void drawAnimatedIcon(int x, int y, int frame);
void drawProgressBar(int x, int y, int width, int height, int progress, int maxProgress);
void drawCornerDecorations();
bool initializeOLED();
bool initializeNFC();
void turnOffOLED();
void turnOnOLED();
void scanI2CDevices();
WakeupReason getWakeupReason();
void configureWakeupSources();
void enterSmartSleep();
void wakeupInitialization();
bool shouldEnterSleep();

/**
 * 检测唤醒原因
 */
WakeupReason getWakeupReason() {
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  
  switch(wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0:
      Serial.println("Wakeup caused by GPIO 26 (Light sensor)");
      return WAKEUP_LIGHT;
      
    case ESP_SLEEP_WAKEUP_EXT1: {
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
      
    case ESP_SLEEP_WAKEUP_TIMER:
      Serial.println("Wakeup caused by timer");
      return WAKEUP_TIMER;
      
    default:
      Serial.println("Wakeup was not caused by deep sleep (first boot or reset)");
      return WAKEUP_UNKNOWN;
  }
}

/**
 * 配置外部唤醒源
 */
void configureWakeupSources() {
  // 使用EXT0唤醒 (单GPIO)
  // GPIO 26 (光敏电阻): 高电平唤醒 (光线充足时)
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_26, 1);  // 1 = 高电平唤醒
  
  // 备用：设置一个较长的定时器唤醒（30分钟）作为安全网
  esp_sleep_enable_timer_wakeup(30 * 60 * 1000000ULL); // 30分钟
  
  Serial.println("Safe wakeup sources configured:");
  Serial.println("- GPIO 26 (LDR): HIGH level (bright light)");
  Serial.println("- Timer: 30 minutes (safety)");
  Serial.println("- GPIO 0 avoided (safe for boot process)");
}

/**
 * 智能睡眠函数
 */
void enterSmartSleep() {
  Serial.println("=== Entering Smart Sleep Mode ===");
  
  // 保存当前状态
  if (isReading || isPaused) {
    unsigned long now = millis();
    unsigned long sessionMillis = accumulatedSessionMillis;
    if (isReading) sessionMillis += now - sessionStartMillis;
    totalReadingSeconds += sessionMillis / 1000;
    
    saveAdaptivePomodoro(sessionMillis);
    prefs.putULong("totalSecs", totalReadingSeconds);
  }
  
  // 显示睡眠界面
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
    delay(2000);
  }
  
  // 关闭外设
  turnOffOLED();
  
  // 配置唤醒源
  configureWakeupSources();
  
  // 进入深度睡眠
  Serial.println("Going to sleep now...");
  esp_deep_sleep_start();
}

/**
 * 光线检测逻辑 - 修复版
 */
bool shouldEnterSleep() {
  static bool sleepCheckEnabled = false;
  static unsigned long systemStartTime = millis();
  static int lowLightCount = 0;
  static unsigned long lastCheck = 0;
  
  unsigned long now = millis();
  int lightValue = analogRead(ldrPin);
  
  // 系统启动后30秒内不检查睡眠（给用户时间操作）
  if (now - systemStartTime < 30000) {
    Serial.println("System startup grace period, sleep check disabled");
    return false;
  }
  
  // 如果正在显示NFC或处于活跃状态，不进入睡眠
  if (isShowingNFC || isReading) {
    lowLightCount = 0;
    return false;
  }
  
  if (now - lastCheck > 2000) { // 每2秒检查一次，避免过于频繁
    lastCheck = now;
    
    Serial.println("Light check - Value: " + String(lightValue) + ", Threshold: " + String(bookmarkThreshold));
    
    if (lightValue < bookmarkThreshold) {
      lowLightCount++;
      Serial.println("Low light detected, count: " + String(lowLightCount) + "/10");
    } else {
      lowLightCount = 0;
      Serial.println("Light sufficient, count reset");
    }
  }
  
  // 需要连续10次检查（20秒）都是低光照才进入睡眠
  if (lowLightCount >= 10) {
    Serial.println("Entering sleep mode due to prolonged low light");
    return true;
  }
  
  return false;
}

/**
 * 从睡眠中恢复的初始化
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
    
    // 根据唤醒原因显示不同信息
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
 * 蜂鸣器基础函数
 */
void beep(int duration) {
  digitalWrite(piezoPin, LOW);
  delay(duration);
  digitalWrite(piezoPin, HIGH);
  Serial.println("Beep! Duration: " + String(duration) + "ms");
}

/**
 * 蜂鸣器提示音函数
 */
void beepNFCSuccess() {
  beep(150);
  delay(100);
  beep(150);
  Serial.println("NFC Success Sound");
}

void beepPomodoroComplete() {
  beep(200);
  delay(150);
  beep(200);
  delay(150);
  beep(300);
  Serial.println("Pomodoro Complete Sound");
}

void beepLightWarning() {
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
 * 自适应番茄钟保存函数
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
  
  adaptivePomodoroMillis = constrain(adaptivePomodoroMillis, 
                                   5 * 60 * 1000UL, 
                                   60 * 60 * 1000UL);
  
  prefs.putULong("pomodoro", adaptivePomodoroMillis);
  prefs.putULong("adaptTime", adaptiveTotalSessionTime);
  prefs.putULong("adaptCount", adaptiveSessionCount);
  
  Serial.println("=== 自适应番茄钟调整 ===");
  Serial.println("完成率: " + String(completionRate * 100, 1) + "%");
  Serial.println("新目标: " + String(adaptivePomodoroMillis / 60000) + "分钟");
  Serial.println("==================");
}

/**
 * 显示NFC检测界面
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
  
  delay(100);
  
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
  
  Serial.println("NFC card detected with beep sound");
}

/**
 * 绘制光线趋势图
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
  display.println("Too dark, find the light");
  
  // 绘制趋势曲线
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
  display.println("v2.2-GPIO");
  
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

void turnOnOLED() {
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(100);
  oledInitialized = initializeOLED();
}

/**
 * setup函数
 */
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Reading Tracker Starting (GPIO Wakeup Version) ===");
  
  // 检查唤醒原因
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED) {
    wakeupInitialization();
  }
  
  // GPIO初始化 (安全版本)
  pinMode(ldrPin, INPUT);           // GPIO 36: ADC输入 (只读，精确测量)
  pinMode(ldrWakeupPin, INPUT);     // GPIO 26: 数字输入 (唤醒用)
  pinMode(buttonWakeupPin, INPUT_PULLUP);  // GPIO 2: 按钮输入 (可选)
  pinMode(piezoPin, OUTPUT);
  digitalWrite(piezoPin, HIGH);
  
  Serial.println("GPIO configuration:");
  Serial.println("- GPIO 36: LDR ADC input (precise measurement)");
  Serial.println("- GPIO 26: LDR digital input (wakeup trigger)");
  Serial.println("- GPIO 2: Button input (optional manual wakeup)");
  Serial.println("- GPIO 0: AVOIDED (safe for boot process)");
  
  // I2C初始化
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(500);
  
  scanI2CDevices();
  
  // 初始化外设
  oledInitialized = initializeOLED();
  if (!oledInitialized) {
    Serial.println("OLED failed, continuing without display...");
  }
  
  prefs.begin("reading", false);
  
  // 加载数据
  totalReadingSeconds = prefs.getULong("totalSecs", 0);
  adaptivePomodoroMillis = prefs.getULong("pomodoro", defaultPomodoroMin * 60000UL);
  adaptiveTotalSessionTime = prefs.getULong("adaptTime", 0);
  adaptiveSessionCount = prefs.getULong("adaptCount", 0);
  
  nfcInitialized = initializeNFC();
  if (!nfcInitialized) {
    Serial.println("NFC failed, continuing without NFC...");
  }
  
  // 显示上次书籍信息
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
    
    for (int i = 0; i < 3; i++) {
      display.drawBitmap(30 + i * 25, 50, icon_star, 16, 16, SSD1306_WHITE);
    }
    
    display.display();
    delay(3000);
  }
  
  Serial.println("=== Setup Complete ===");
  Serial.print("OLED: ");
  Serial.println(oledInitialized ? "OK" : "Failed");
  Serial.print("NFC: ");
  Serial.println(nfcInitialized ? "OK" : "Failed");
  
  beep(200);
  delay(100);
  beep(200);
  Serial.println("System ready with GPIO wakeup support!");
}

/**
 * 主循环 - 修复版
 */
void loop() {
  int lightValue = analogRead(ldrPin);
  lightTrend[trendIndex] = lightValue;
  trendIndex = (trendIndex + 1) % trendSize;

  if (isReading) {
    bestLuxEMA = (1 - emaAlpha) * bestLuxEMA + emaAlpha * lightValue;
    bestLuxEMA = constrain(bestLuxEMA, 100, 3000);
  }

  unsigned long now = millis();
  
  // 添加调试信息
  static unsigned long lastDebugPrint = 0;
  if (now - lastDebugPrint > 5000) { // 每5秒打印一次状态
    lastDebugPrint = now;
    Serial.println("=== System Status ===");
    Serial.println("Light Value: " + String(lightValue));
    Serial.println("Is Reading: " + String(isReading));
    Serial.println("Is Showing NFC: " + String(isShowingNFC));
    Serial.println("In Rest: " + String(inRest));
    Serial.println("==================");
  }
  
  // NFC检测逻辑 - 移到睡眠检测之前
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
    } else {
      if (isShowingNFC) {
        isShowingNFC = false;
        Serial.println("NFC card removed, returning to normal mode");
      }
      i2cBusy = false;
    }
  }
  
  if (isShowingNFC) {
    if (now - nfcDisplayStartTime > 30000) {
      isShowingNFC = false;
      Serial.println("NFC display timeout, returning to normal mode");
    } else {
      delay(500);
      return;
    }
  }

  // 智能睡眠检测 - 移到最后，并且只在非活跃状态下检查
  if (!isReading && !inRest && !isShowingNFC && shouldEnterSleep()) {
    Serial.println("All conditions met for sleep, entering sleep mode...");
    enterSmartSleep();
  }
  
  // 阅读逻辑
  if (lightValue < lightThreshold) {
    if (isReading) {
      accumulatedSessionMillis += now - sessionStartMillis;
      isReading = false;
      isPaused = true;
      Serial.println("Reading paused due to low light");
    }
    
    // 只有在不显示NFC时才显示趋势图
    if (!isShowingNFC) {
      drawTrendGraph();
      beepLightWarning();
    }
  } else {
    // 正常阅读模式
    if (!isReading && !inRest) {
      isReading = true;
      isPaused = false;
      sessionStartMillis = now;
      Serial.println("Reading session started");
    }
    
    if (isReading && oledInitialized && !i2cBusy && !isShowingNFC) {
      unsigned long currentSessionMillis = now - sessionStartMillis + accumulatedSessionMillis;
      unsigned long targetMillis = adaptivePomodoroMillis;
      
      display.clearDisplay();
      drawCornerDecorations();
      
      // 显示阅读信息
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
      
      // 进度条
      int progress = (currentSessionMillis * 100) / targetMillis;
      progress = constrain(progress, 0, 100);
      drawProgressBar(6, 50, 115, 8, progress, 100);
      
      display.display();
      
      // 检查是否完成番茄钟
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
  
  // 休息模式
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
  
  delay(500);  // 主循环延迟
}