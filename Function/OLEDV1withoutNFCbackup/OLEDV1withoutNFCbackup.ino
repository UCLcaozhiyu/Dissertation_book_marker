#include <Arduino.h>
#include "driver/ledc.h"
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "esp_system.h"
#include "esp_sleep.h"
#include <Adafruit_PN532.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#define SDA_PIN 8
#define SCL_PIN 9
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

bool initializeOLED() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("OLED initialization failed!");
    return false;
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("OLED OK!");
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

void drawBookAnimation(int frame) {
  if (!oledInitialized) return;
  
  int baseX = 110, baseY = 10;
  switch (frame % 3) {
    case 0:
      display.fillRect(baseX, baseY, 6, 10, SSD1306_WHITE);
      break;
    case 1:
      display.fillRect(baseX, baseY, 6, 10, SSD1306_WHITE);
      display.drawLine(baseX + 6, baseY, baseX + 10, baseY + 5, SSD1306_WHITE);
      display.drawLine(baseX + 10, baseY + 5, baseX + 6, baseY + 10, SSD1306_WHITE);
      display.drawLine(baseX + 6, baseY + 10, baseX + 6, baseY, SSD1306_WHITE);
      break;
    case 2:
      display.drawRect(baseX, baseY, 6, 10, SSD1306_WHITE);
      display.drawLine(baseX + 6, baseY, baseX + 10, baseY + 5, SSD1306_WHITE);
      display.drawLine(baseX + 10, baseY + 5, baseX + 6, baseY + 10, SSD1306_WHITE);
      break;
  }
}

void drawTrendGraph() {
  if (!oledInitialized || i2cBusy) return;  // 检查I2C总线状态
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Light Trend");
  display.setCursor(0, 10);
  display.println("Best Lux: " + String((int)bestLuxEMA));
  
  for (int i = 0; i < trendSize - 1; i++) {
    int x1 = i * (SCREEN_WIDTH / trendSize);
    int y1 = SCREEN_HEIGHT - 1 - map(lightTrend[(trendIndex + i) % trendSize], 0, 4095, 0, SCREEN_HEIGHT - 20);
    int x2 = (i + 1) * (SCREEN_WIDTH / trendSize);
    int y2 = SCREEN_HEIGHT - 1 - map(lightTrend[(trendIndex + i + 1) % trendSize], 0, 4095, 0, SCREEN_HEIGHT - 20);
    display.drawLine(x1, y1, x2, y2, SSD1306_WHITE);
  }
  display.display();
}

void beep(int duration = 100) {
  // 注释掉蜂鸣器功能，避免LEDC问题
  // ledcWrite(0, 127);
  // delay(duration);
  // ledcWrite(0, 0);
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
    display.setCursor(0, 0);
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.println("Book Detected!");
    display.println("UID:");
    display.println(uidStr);
    display.println("Welcome back!");
    display.display();
  }
  
  // 播放提示音表示检测到卡片
  Serial.println("Beep! NFC card detected");
  delay(2000);
}

void setup() {
  Serial.begin(115200);
  delay(500);  // 减少延迟，提高响应速度
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
    display.setCursor(0, 0);
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.println("Last NFC UID:");
    display.println(lastUID);
    display.println("Welcome back!");
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
    
    if (oledInitialized && !i2cBusy) {  // 检查I2C总线状态
      display.clearDisplay();
      display.setCursor(0, 0);
      display.setTextColor(SSD1306_WHITE);
      display.setTextSize(1);
      display.println("Going to sleep...");
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
        if (oledInitialized && !i2cBusy) {  // 检查I2C总线状态
          display.clearDisplay();
          display.setCursor(0, 0);
          display.setTextColor(SSD1306_WHITE);
          display.setTextSize(1);
          display.println("Resting...");
          display.println("Take your time.");
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
        if (oledInitialized && !i2cBusy) {  // 检查I2C总线状态
          int cm = sessionMillis / 60000;
          int cs = (sessionMillis / 1000) % 60;
          unsigned long totalMinutes = totalReadingSeconds / 60;
          unsigned long totalHours = totalMinutes / 60;
          unsigned long totalMinsOnly = totalMinutes % 60;
          int pm = adaptivePomodoroMillis / 60000;
          String totalStr = (totalHours > 0) ? (String(totalHours) + "h " + String(totalMinsOnly) + "m") : (String(totalMinsOnly) + "m");
          
          display.clearDisplay();
          display.setCursor(0, 0);
          display.setTextColor(SSD1306_WHITE);
          display.setTextSize(1);
          display.println("Reading: " + String(cm) + "m " + String(cs) + "s");
          display.println("Total: " + totalStr);
          display.println("Pomodoro: " + String(pm) + "m");
          display.println("OLED: OK, NFC: " + String(nfcInitialized ? "OK" : "Failed"));
          drawBookAnimation(animationFrame++);
          display.display();
        }
      }
    }
  }
  
  delay(1000);
}