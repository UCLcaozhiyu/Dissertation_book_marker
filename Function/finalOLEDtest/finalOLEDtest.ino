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

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

// === PN532 NFC 配置 ===
#define SDA_PIN 8
#define SCL_PIN 9

// ===== 像素图标定义 (16x16) - 必须在最前面 =====
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

// === 对象声明 ===
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_PN532 nfc(SDA_PIN, SCL_PIN);

String lastUID = "";
bool showLastUIDOnBoot = false;
bool nfcInitialized = false;
bool oledInitialized = false;
bool i2cBusy = false;  // I2C总线互斥标志

const int ldrPin = 0;
const int piezoPin = 5;

const int lightThreshold = 1000;
const int bookmarkThreshold = 50;
const uint64_t uS_TO_S_FACTOR = 1000000ULL;
const int TIME_TO_SLEEP = 10;

// === Pomodoro 配置 ===
const int defaultPomodoroMin = 25;
unsigned long adaptivePomodoroMillis = defaultPomodoroMin * 60000UL;
unsigned long adaptiveTotalSessionTime = 0;
unsigned long adaptiveSessionCount = 0;

const unsigned long restDuration = 5 * 60 * 1000UL;

bool isReading = false;
bool isPaused = false;
bool inRest = false;
bool isSleeping = false;

unsigned long sessionStartMillis = 0;
unsigned long accumulatedSessionMillis = 0;
unsigned long restStartMillis = 0;
unsigned long totalReadingSeconds = 0;

Preferences prefs;

const int trendSize = 30;
int lightTrend[trendSize] = {0};
int trendIndex = 0;
float bestLuxEMA = 0.0;
const float emaAlpha = 0.05;
int animationFrame = 0;

// I2C设备扫描函数
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

// 装饰边框函数
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

// 绘制动态图标
void drawAnimatedIcon(int x, int y, int frame) {
  const unsigned char* icons[] = {icon_book, icon_heart, icon_star, icon_clock};
  int iconIndex = (frame / 20) % 4; // 每20帧切换图标
  
  display.drawBitmap(x, y, icons[iconIndex], 16, 16, SSD1306_WHITE);
}

// 绘制进度条
void drawProgressBar(int x, int y, int width, int height, int progress, int maxProgress) {
  display.drawRect(x, y, width, height, SSD1306_WHITE);
  int fillWidth = (progress * (width - 2)) / maxProgress;
  if (fillWidth > 0) {
    display.fillRect(x + 1, y + 1, fillWidth, height - 2, SSD1306_WHITE);
  }
}

bool initializeOLED() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("OLED initialization failed!");
    return false;
  }
  
  // 启动动画
  display.clearDisplay();
  
  // 绘制装饰边框
  drawCornerDecorations();
  
  // 标题
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(15, 2);
  display.println("Reading Tracker");
  
  // 中央图标组合
  display.drawBitmap(30, 20, icon_book, 16, 16, SSD1306_WHITE);
  display.drawBitmap(50, 20, icon_heart, 16, 16, SSD1306_WHITE);
  display.drawBitmap(70, 20, icon_clock, 16, 16, SSD1306_WHITE);
  
  // 版本信息
  display.setCursor(45, 45);
  display.println("v2.0");
  
  // 装饰点
  for (int i = 0; i < SCREEN_WIDTH; i += 8) {
    display.drawPixel(i, 55, SSD1306_WHITE);
  }
  
  display.display();
  delay(2000);
  
  // 第二屏 - 初始化信息
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

String getUIDString(uint8_t* uid, uint8_t uidLength) {
  String uidStr = "";
  for (uint8_t i = 0; i < uidLength; i++) {
    if (uid[i] < 0x10) uidStr += "0";
    uidStr += String(uid[i], HEX);
    if (i < uidLength - 1) uidStr += "";
  }
  uidStr.toUpperCase();
  return uidStr;
}

void loadBookData(String bookUID) {
  String key = "book_" + bookUID;
  // currentBookSeconds = prefs.getULong(key.c_str(), 0);
  
  // 尝试获取书名，如果没有则使用默认名称
  String nameKey = "name_" + bookUID;
  // currentBookName = prefs.getString(nameKey.c_str(), "Book " + bookUID.substring(0, 4));
}

void saveBookData(String bookUID, unsigned long seconds) {
  String key = "book_" + bookUID;
  prefs.putULong(key.c_str(), seconds);
  Serial.println("Saved book " + bookUID + ": " + String(seconds) + " seconds");
}

void saveBookName(String bookUID, String bookName) {
  String nameKey = "name_" + bookUID;
  prefs.putString(nameKey.c_str(), bookName);
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

void drawTrendGraph() {
  if (!oledInitialized || i2cBusy) return;
  
  display.clearDisplay();
  
  // 绘制装饰边框
  drawCornerDecorations();
  
  // 标题和图标
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(20, 2);
  display.println("Please do not read in the dark.");
  display.println("Look for light. Light Trend");
  
  // 绘制图表图标
  display.drawBitmap(2, 2, icon_battery, 16, 16, SSD1306_WHITE);
  
  // 数据显示
  display.setCursor(20, 12);
  display.println("Lux: " + String((int)bestLuxEMA));
  
  // 绘制趋势线
  for (int i = 0; i < trendSize - 1; i++) {
    int x1 = i * (SCREEN_WIDTH / trendSize);
    int y1 = SCREEN_HEIGHT - 8 - map(lightTrend[(trendIndex + i) % trendSize], 0, 4095, 0, 35);
    int x2 = (i + 1) * (SCREEN_WIDTH / trendSize);
    int y2 = SCREEN_HEIGHT - 8 - map(lightTrend[(trendIndex + i + 1) % trendSize], 0, 4095, 0, 35);
    display.drawLine(x1, y1, x2, y2, SSD1306_WHITE);
  }
  
  // 底部装饰线
  for (int i = 0; i < SCREEN_WIDTH; i += 4) {
    display.drawPixel(i, SCREEN_HEIGHT - 2, SSD1306_WHITE);
  }
  
  display.display();
}

void beep(int duration = 100) {
  // 注释掉蜂鸣器功能，避免LEDC问题
  // ledcWrite(piezoPin, 127);
  // delay(duration);
  // ledcWrite(piezoPin, 0);
  Serial.println("Beep!");
}

void saveAdaptivePomodoro(unsigned long sessionMillis) {
  if (sessionMillis < 5 * 60 * 1000UL) return;
  if (sessionMillis >= adaptivePomodoroMillis) {
    adaptivePomodoroMillis += 5 * 60 * 1000UL;
    adaptivePomodoroMillis = min(adaptivePomodoroMillis, 60 * 60 * 1000UL);
  } else {
    adaptiveTotalSessionTime += sessionMillis;
    adaptiveSessionCount++;
    unsigned long averageMillis = adaptiveTotalSessionTime / adaptiveSessionCount;
    adaptivePomodoroMillis = constrain(averageMillis, 5 * 60 * 1000UL, 60 * 60 * 1000UL);
  }
  prefs.putULong("pomodoro", adaptivePomodoroMillis);
  prefs.putULong("adaptTime", adaptiveTotalSessionTime);
  prefs.putULong("adaptCount", adaptiveSessionCount);
  Serial.print("[Pomodoro Adapted]: ");
  Serial.print(adaptivePomodoroMillis / 60000);
  Serial.println(" min");
}

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
  
  // 暂停一下，让I2C总线稳定
  delay(100);
  
  if (oledInitialized) {
    display.clearDisplay();
    
    // 绘制装饰边框
    drawCornerDecorations();
    
    // 绘制书本图标
    display.drawBitmap(6, 10, icon_book, 16, 16, SSD1306_WHITE);
    
    // 文字内容
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(28, 2);
    display.println("Book Found!");
    
    display.setCursor(28, 12);
    display.println("ID: " + uidStr.substring(0, 8));
    display.setCursor(28, 22);
    display.println("    " + uidStr.substring(9));
    
    // 绘制欢迎图标
    display.drawBitmap(106, 10, icon_heart, 16, 16, SSD1306_WHITE);
    
    // 底部欢迎信息
    display.setCursor(25, 45);
    display.println("Welcome Back!");
    
    // 装饰点
    for (int i = 0; i < 5; i++) {
      display.fillCircle(20 + i * 20, 55, 1, SSD1306_WHITE);
    }
    
    display.display();
  }
  
  // 播放提示音表示检测到卡片
  Serial.println("Beep! NFC card detected");
  delay(2000);
}

bool checkNFCCard() {
  if (!nfcInitialized) return false;
  
  uint8_t uid[7];
  uint8_t uidLength;
  
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 100)) {
    String newUID = getUIDString(uid, uidLength);
    
    if (newUID != lastUID) {
      lastUID = newUID;
      Serial.println("Detected book: " + lastUID);
      
      // 播放提示音
      beep(200);
      delay(100);
      beep(200);
      
      return true;
    }
    return true; // 同一张卡
  }
  
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Reading Tracker Starting ===");
  
  pinMode(ldrPin, INPUT);
  pinMode(piezoPin, OUTPUT);
  digitalWrite(piezoPin, LOW);
  
  // 注释掉LEDC初始化，避免蜂鸣器问题
  // ledcSetup(0, 3000, 8);
  // ledcAttachPin(piezoPin, 0);
  
  // 初始化I2C和设备
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(500);
  
  // 扫描I2C设备
  scanI2CDevices();
  
  // 初始化OLED
  oledInitialized = initializeOLED();
  if (!oledInitialized) {
    Serial.println("OLED failed, continuing without display...");
  }
  
  // 初始化Preferences
  prefs.begin("reading", false);
  totalReadingSeconds = prefs.getULong("totalSecs", 0);
  adaptivePomodoroMillis = prefs.getULong("pomodoro", defaultPomodoroMin * 60000UL);
  adaptiveTotalSessionTime = prefs.getULong("adaptTime", 0);
  adaptiveSessionCount = prefs.getULong("adaptCount", 0);
  
  // 初始化NFC
  nfcInitialized = initializeNFC();
  if (!nfcInitialized) {
    Serial.println("NFC failed, continuing without NFC...");
    if (oledInitialized) {
      display.clearDisplay();
      display.setCursor(0, 0);
      display.println("NFC Failed!");
      display.println("Check connections");
      display.display();
    }
  }
  
  // 显示上次的UID
  lastUID = prefs.getString("lastUID", "");
  if (lastUID.length() > 0 && oledInitialized) {
    display.clearDisplay();
    
    // 绘制装饰边框
    drawCornerDecorations();
    
    // 欢迎回来页面
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(25, 2);
    display.println("Welcome Back!");
    
    // 书本图标
    display.drawBitmap(6, 15, icon_book, 16, 16, SSD1306_WHITE);
    
    // 上次UID信息
    display.setCursor(28, 15);
    display.println("Last Book:");
    display.setCursor(28, 25);
    display.println(lastUID.substring(0, 8));
    
    // 心形图标
    display.drawBitmap(106, 15, icon_heart, 16, 16, SSD1306_WHITE);
    
    // 装饰星星
    for (int i = 0; i < 3; i++) {
      display.drawBitmap(30 + i * 25, 45, icon_star, 16, 16, SSD1306_WHITE);
    }
    
    display.display();
    delay(3000);
  }
  
  Serial.println("=== Setup Complete ===");
  Serial.print("OLED: ");
  Serial.println(oledInitialized ? "OK" : "Failed");
  Serial.print("NFC: ");
  Serial.println(nfcInitialized ? "OK" : "Failed");
}

void loop() {
  int lightValue = analogRead(ldrPin);
  lightTrend[trendIndex] = lightValue;
  trendIndex = (trendIndex + 1) % trendSize;

  if (isReading) {
    bestLuxEMA = (1 - emaAlpha) * bestLuxEMA + emaAlpha * lightValue;
    bestLuxEMA = constrain(bestLuxEMA, 100, 3000);
  }

  unsigned long now = millis();
  
  // NFC检测 (降低频率，避免I2C冲突)
  static unsigned long lastNFCCheck = 0;
  if (nfcInitialized && !i2cBusy && (now - lastNFCCheck > 3000)) {  // 每3秒检查一次
    lastNFCCheck = now;
    i2cBusy = true;  // 锁定I2C总线
    
    uint8_t uid[7];
    uint8_t uidLength;
    
    // 读取NFC时使用更长的超时时间，减少重试
    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 1000)) {
      showNFCUIDAndSave(uid, uidLength);
      i2cBusy = false;  // 释放I2C总线
      delay(2000); // 显示完成后等待，避免连续读取
      return;
    }
    
    i2cBusy = false;  // 释放I2C总线
  }

  // 检查是否进入睡眠模式
  if (lightValue < bookmarkThreshold) {
    if (isReading || isPaused) {
      unsigned long sessionMillis = accumulatedSessionMillis;
      if (isReading) sessionMillis += now - sessionStartMillis;
      totalReadingSeconds += sessionMillis / 1000;
      
      saveAdaptivePomodoro(sessionMillis);
      prefs.putULong("totalSecs", totalReadingSeconds);
    }
    
    if (oledInitialized && !i2cBusy) {
      display.clearDisplay();
      
      // 绘制装饰边框
      drawCornerDecorations();
      
      // 标题
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(25, 2);
      display.println("Good Night!");
      
      // 电池图标（表示睡眠）
      display.drawBitmap(6, 20, icon_battery, 16, 16, SSD1306_WHITE);
      
      // 睡眠信息
      display.setCursor(28, 20);
      display.println("Sleeping in");
      display.setCursor(35, 30);
      display.println(String(TIME_TO_SLEEP) + " seconds");
      
      // 时钟图标
      display.drawBitmap(106, 20, icon_clock, 16, 16, SSD1306_WHITE);
      
      // 底部装饰 - Z字符表示睡眠
      display.setCursor(45, 50);
      display.setTextSize(2);
      display.println("Z z z");
      
      display.display();
      delay(1000);
    }
    
    turnOffOLED();
    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
    esp_deep_sleep_start();
  }
  
  // 检查光线阈值
  else if (lightValue < lightThreshold) {
    if (isReading) {
      accumulatedSessionMillis += now - sessionStartMillis;
      isReading = false;
      isPaused = true;
    }
    drawTrendGraph();
    beep(1000);
  }
  
  // 正常阅读逻辑
  else {
    if (!isReading) {
      if (inRest) {
        if (restStartMillis == 0) restStartMillis = now;
        if (now - restStartMillis >= restDuration) {
          inRest = false;
          beep(300);
          sessionStartMillis = now;
          accumulatedSessionMillis = 0;
          isReading = true;
        } else {
          if (oledInitialized && !i2cBusy) {
            display.clearDisplay();
            
            // 绘制装饰边框
            drawCornerDecorations();
            
            // 标题和图标
            display.setTextSize(1);
            display.setTextColor(SSD1306_WHITE);
            display.setCursor(30, 2);
            display.println("Rest Time");
            
            // 左侧心形图标
            display.drawBitmap(6, 15, icon_heart, 16, 16, SSD1306_WHITE);
            
            // 中央信息
            display.setTextSize(1);
            display.setCursor(28, 20);
            display.println("Take a break!");
            display.setCursor(35, 30);
            display.println("Relax...");
            
            // 右侧星星图标
            display.drawBitmap(106, 15, icon_star, 16, 16, SSD1306_WHITE);
            
            // 计算剩余休息时间
            unsigned long restRemaining = restDuration - (now - restStartMillis);
            int restMin = restRemaining / 60000;
            int restSec = (restRemaining / 1000) % 60;
            
            display.setCursor(35, 45);
            display.println(String(restMin) + ":" + (restSec < 10 ? "0" : "") + String(restSec));
            
            // 装饰波浪线
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
        sessionStartMillis = now;
        isPaused = false;
        isReading = true;
      }
    } else {
      unsigned long sessionMillis = accumulatedSessionMillis + (now - sessionStartMillis);
      if (sessionMillis >= adaptivePomodoroMillis) {
        inRest = true;
        isReading = false;
        restStartMillis = 0;
        totalReadingSeconds += sessionMillis / 1000;
        
        saveAdaptivePomodoro(sessionMillis);
        prefs.putULong("totalSecs", totalReadingSeconds);
        beep(500);
      } else {
        if (oledInitialized && !i2cBusy) {
          int cm = sessionMillis / 60000;
          int cs = (sessionMillis / 1000) % 60;
          unsigned long totalMinutes = totalReadingSeconds / 60;
          unsigned long totalHours = totalMinutes / 60;
          unsigned long totalMinsOnly = totalMinutes % 60;
          int pm = adaptivePomodoroMillis / 60000;
          String totalStr = (totalHours > 0) ? (String(totalHours) + "h " + String(totalMinsOnly) + "m") : (String(totalMinsOnly) + "m");
          
          display.clearDisplay();
          
          // 绘制装饰边框
          drawCornerDecorations();
          
          // 标题区域
          display.setTextSize(1);
          display.setTextColor(SSD1306_WHITE);
          display.setCursor(22, 2);
          display.println("Reading Mode");
          
          // 左侧时钟图标
          display.drawBitmap(2, 2, icon_clock, 16, 16, SSD1306_WHITE);
          
          // 当前会话时间 (大字体显示)
          display.setTextSize(2);
          display.setCursor(8, 15);
          display.print(String(cm) + ":" + (cs < 10 ? "0" : "") + String(cs));
          
          // 右侧动态图标
          drawAnimatedIcon(108, 15, animationFrame);
          
          // 统计信息
          display.setTextSize(1);
          display.setCursor(8, 35);
          display.println("Total: " + totalStr);
          
          display.setCursor(8, 45);
          display.println("Target: " + String(pm) + " min");
          
          // 进度条显示番茄钟进度
          int progress = (sessionMillis * 100) / adaptivePomodoroMillis;
          drawProgressBar(8, 55, 112, 6, progress, 100);
          
          // 系统状态指示点
          display.fillCircle(120, 45, 2, nfcInitialized ? SSD1306_WHITE : SSD1306_BLACK);
          display.drawCircle(120, 45, 2, SSD1306_WHITE);
          
          display.display();
          animationFrame++;
        }
      }
    }
  }
  
  delay(500);  // 减少延迟，提高响应速度
}