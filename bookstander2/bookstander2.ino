#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <SPI.h>
#include "esp_system.h"
#include "esp_sleep.h"

// NFC I2C引脚
#define SDA_PIN 8
#define SCL_PIN 9

// e-paper SPI引脚 (重新分配避免冲突)
#define EPD_CS   10
#define EPD_DC   4
#define EPD_RST  5
#define EPD_BUSY 3
#define EPD_MOSI 6
#define EPD_SCK  7

Adafruit_PN532 nfc(SDA_PIN, SCL_PIN);
GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> display(GxEPD2_154_D67(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

const int ldrPin = 0;  // 光感传感器
const int piezoPin = 1;  // 改为PIN1，避免与EPD_RST冲突

const int lightThreshold = 1000;
const int bookmarkThreshold = 50;
const uint64_t uS_TO_S_FACTOR = 1000000ULL;
const int TIME_TO_SLEEP = 10;

const int defaultPomodoroMin = 25;
unsigned long adaptivePomodoroMillis = defaultPomodoroMin * 60000UL;
unsigned long adaptiveTotalSessionTime = 0;
unsigned long adaptiveSessionCount = 0;

const unsigned long restDuration = 5 * 60 * 1000UL;

bool isReading = false;
bool isPaused = false;
bool inRest = false;
bool isSleeping = false;
bool nfcDetected = false;
unsigned long nfcDisplayStart = 0;
String lastBookInfo = "";

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
int refreshCount = 0;

bool justWokeUp = false;
static bool justStartedReading = false;

void toneManual(int pin, int frequency, int duration) {
  long period = 1000000L / frequency;
  long cycles = frequency * duration / 1000;
  for (long i = 0; i < cycles; i++) {
    digitalWrite(pin, HIGH);
    delayMicroseconds(period / 2);
    digitalWrite(pin, LOW);
    delayMicroseconds(period / 2);
  }
}

void beep(int duration = 100) {
  toneManual(piezoPin, 1000, duration);
}

// 页面类型枚举
enum PageType {
    PAGE_READING,
    PAGE_DARK,
    PAGE_NFC,
    PAGE_REST,
    PAGE_DEFAULT
};

// 放大版图标
void drawBookIconLarge() {
    display.fillRect(10, 10, 40, 32, GxEPD_BLACK);
    display.drawRect(10, 10, 40, 32, GxEPD_BLACK);
    display.drawLine(30, 10, 30, 42, GxEPD_WHITE);
}
void drawBulbIconLarge() {
    display.drawCircle(30, 26, 18, GxEPD_BLACK);
    display.drawLine(30, 44, 30, 60, GxEPD_BLACK);
    display.drawLine(20, 55, 40, 55, GxEPD_BLACK);
}
void drawNFCIconLarge() {
    display.drawRect(15, 15, 36, 36, GxEPD_BLACK);
    display.drawLine(20, 20, 46, 46, GxEPD_BLACK);
    display.drawLine(46, 20, 20, 46, GxEPD_BLACK);
}
void drawCoffeeIconLarge() {
    display.drawRect(15, 40, 36, 20, GxEPD_BLACK);
    display.drawCircle(33, 40, 18, GxEPD_BLACK);
    display.drawLine(51, 45, 60, 55, GxEPD_BLACK);
}
void drawSmileIconLarge() {
    display.drawCircle(30, 26, 18, GxEPD_BLACK);
    display.drawPixel(22, 20, GxEPD_BLACK);
    display.drawPixel(38, 20, GxEPD_BLACK);
    display.drawLine(24, 38, 36, 38, GxEPD_BLACK); // 微笑
}

// 底部大图标
void drawBookIconBottom() {
    display.fillRect(60, 130, 80, 50, GxEPD_BLACK);
    display.drawRect(60, 130, 80, 50, GxEPD_BLACK);
    display.drawLine(100, 130, 100, 180, GxEPD_WHITE);
}
void drawBulbIconBottom() {
    display.drawCircle(100, 160, 30, GxEPD_BLACK);
    display.drawLine(100, 190, 100, 200, GxEPD_BLACK);
    display.drawLine(80, 195, 120, 195, GxEPD_BLACK);
}
void drawNFCIconBottom() {
    display.drawRect(70, 140, 60, 40, GxEPD_BLACK);
    display.drawLine(80, 150, 120, 180, GxEPD_BLACK);
    display.drawLine(120, 150, 80, 180, GxEPD_BLACK);
}
void drawCoffeeIconBottom() {
    display.drawRect(75, 160, 50, 25, GxEPD_BLACK);
    display.drawCircle(100, 160, 25, GxEPD_BLACK);
    display.drawLine(125, 170, 140, 185, GxEPD_BLACK);
}
void drawSmileIconBottom() {
    display.drawCircle(100, 160, 30, GxEPD_BLACK);
    display.drawPixel(90, 150, GxEPD_BLACK);
    display.drawPixel(110, 150, GxEPD_BLACK);
    display.drawLine(92, 175, 108, 175, GxEPD_BLACK);
}

// 智能e-paper显示函数，文字左对齐上半部分，图片底部居中
void showOnEInkSmart(String msg, PageType type = PAGE_DEFAULT) {
    display.setFullWindow(); // 每次都全屏刷新
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        // 文字部分（左对齐，上半部分）
        display.setTextColor(GxEPD_BLACK);
        display.setFont(&FreeMonoBold12pt7b);
        int16_t y = 30; // 上半部分
        int16_t lineHeight = 32;
        int16_t start = 0, end = 0;
        while ((end = msg.indexOf('\n', start)) != -1) {
            display.setCursor(10, y); // 左对齐
            display.print(msg.substring(start, end));
            y += lineHeight;
            start = end + 1;
        }
        display.setCursor(10, y);
        display.print(msg.substring(start));
        // 图片部分（下半部分居中）
        switch(type) {
            case PAGE_READING: drawBookIconBottom(); break;
            case PAGE_DARK: drawBulbIconBottom(); break;
            case PAGE_NFC: drawNFCIconBottom(); break;
            case PAGE_REST: drawCoffeeIconBottom(); break;
            default: drawSmileIconBottom(); break;
        }
    } while (display.nextPage());
}

void drawBookAnimation(int frame) {
  // 简化的书本动画，适合e-paper
  int baseX = 110, baseY = 10;
  switch (frame % 3) {
    case 0:
      display.fillRect(baseX, baseY, 6, 10, GxEPD_BLACK); break;
    case 1:
      display.drawRect(baseX, baseY, 6, 10, GxEPD_BLACK);
      display.drawLine(baseX + 6, baseY, baseX + 10, baseY + 5, GxEPD_BLACK);
      break;
    case 2:
      display.drawRect(baseX, baseY, 6, 10, GxEPD_BLACK);
      break;
  }
}

void drawNFCInfo() {
  String nfcMsg = "Book Info:\n" + lastBookInfo;
  showOnEInkSmart(nfcMsg, PAGE_NFC);
}

void displayStatus(const String& line1, const String& line2 = "", const String& line3 = "") {
  String statusMsg = line1;
  if (line2 != "") statusMsg += "\n" + line2;
  if (line3 != "") statusMsg += "\n" + line3;
  showOnEInkSmart(statusMsg, PAGE_DEFAULT);
}

void enterDeepSleep() {
  unsigned long waitStart = millis();
  while (millis() - waitStart < 2000) {
    uint8_t uid[] = {0, 0, 0, 0, 0, 0, 0};
    uint8_t uidLength;
    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength)) {
      String uidStr = "";
      for (uint8_t i = 0; i < uidLength; i++) {
        if (uid[i] < 0x10) uidStr += "0";
        uidStr += String(uid[i], HEX);
      }
      lastBookInfo = uidStr;
      prefs.putString("lastBook", lastBookInfo);
      break;
    }
    delay(100);
  }

  isSleeping = true;
  displayStatus("Bookmark in", "Saving & sleep...");
  prefs.putULong("totalSecs", totalReadingSeconds);
  prefs.putULong("pomodoro", adaptivePomodoroMillis);
  prefs.putULong("adaptTime", adaptiveTotalSessionTime);
  prefs.putULong("adaptCount", adaptiveSessionCount);
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
  esp_deep_sleep_start();
}

void updateAdaptivePomodoro(unsigned long sessionMillis) {
  if (sessionMillis >= adaptivePomodoroMillis) {
    adaptivePomodoroMillis = min(adaptivePomodoroMillis + 5 * 60000UL, 60 * 60000UL);
  } else if (sessionMillis >= 5 * 60000UL) {
    adaptivePomodoroMillis = max(adaptivePomodoroMillis - 2 * 60000UL, 5 * 60000UL);
  }
  adaptiveTotalSessionTime += sessionMillis;
  adaptiveSessionCount++;
}

void updateLightTrend(int lightVal) {
  lightTrend[trendIndex] = lightVal;
  trendIndex = (trendIndex + 1) % trendSize;
  if (isReading) {
    bestLuxEMA = emaAlpha * lightVal + (1.0 - emaAlpha) * bestLuxEMA;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(ldrPin, INPUT);
  pinMode(piezoPin, OUTPUT);
  digitalWrite(piezoPin, LOW);

  // 初始化e-paper
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init();
  display.setRotation(1);

  // 初始化NFC
  Wire.begin(SDA_PIN, SCL_PIN);
  nfc.begin();
  
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("Didn't find PN532 board");
    showOnEInkSmart("Didn't find\nPN532 board", PAGE_NFC);
    while (1);
  }

  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
    justWokeUp = true;
  }

  prefs.begin("reading", false);
  totalReadingSeconds = prefs.getULong("totalSecs", 0);
  adaptivePomodoroMillis = prefs.getULong("pomodoro", defaultPomodoroMin * 60000UL);
  adaptiveTotalSessionTime = prefs.getULong("adaptTime", 0);
  adaptiveSessionCount = prefs.getULong("adaptCount", 0);
  lastBookInfo = prefs.getString("lastBook", "");

  if (justWokeUp && lastBookInfo != "") {
    drawNFCInfo();
    delay(5000);
  } else {
    showOnEInkSmart("BookStander Ready\nWaiting for book...", PAGE_DEFAULT);
  }
}

void loop() {
  if (nfcDetected) {
    drawNFCInfo();
    if (millis() - nfcDisplayStart > 5000) nfcDetected = false;
    delay(100);
    return;
  }

  int lightValue = analogRead(ldrPin);
  Serial.print("LDR Value: ");
  Serial.println(lightValue);
  unsigned long now = millis();
  if (lightValue >= lightThreshold) updateLightTrend(lightValue);

  if (lightValue < bookmarkThreshold) {
    if (isReading || isPaused) {
      unsigned long sessionMillis = accumulatedSessionMillis;
      if (isReading) sessionMillis += now - sessionStartMillis;
      totalReadingSeconds += sessionMillis / 1000;
      updateAdaptivePomodoro(sessionMillis);
      accumulatedSessionMillis = 0;
    }
    enterDeepSleep();
    return;
  }

  if (lightValue < lightThreshold) {
    if (isReading) {
      accumulatedSessionMillis += now - sessionStartMillis;
      isReading = false;
      isPaused = true;
      displayStatus("Low light.", "Please read in light place.", "Protect your eyes!");
    } else {
      displayStatus("Paused", "Waiting for   light...");
    }
    delay(1000);
    return;
  }

  if (!isReading) {
    if (inRest) {
      if (now - restStartMillis >= restDuration) {
        inRest = false;
        displayStatus("Rest done.", "Back to reading");
        beep(300);
        sessionStartMillis = now;
        accumulatedSessionMillis = 0;
        isReading = true;
      } else {
        displayStatus("Resting...", "Enjoy your break");
        delay(1000);
        return;
      }
    } else {
      if (!justStartedReading) {
        sessionStartMillis = now;
        isPaused = false;
        isReading = true;
        showOnEInkSmart("Good light\nStart reading", PAGE_READING);
        delay(2000); // 只显示2秒
        justStartedReading = true;
      }
      // 不再重复显示good light页面
    }
  } else {
    justStartedReading = false;
    unsigned long sessionMillis = accumulatedSessionMillis + (now - sessionStartMillis);
    if (sessionMillis >= adaptivePomodoroMillis) {
      totalReadingSeconds += sessionMillis / 1000;
      updateAdaptivePomodoro(sessionMillis);
      prefs.putULong("totalSecs", totalReadingSeconds);
      accumulatedSessionMillis = 0;
      isReading = false;
      isPaused = false;
      inRest = true;
      displayStatus("Pomodoro done!", "Take a break");
      beep(500);
      restStartMillis = now;
      delay(3000);
    } else {
      // 显示阅读状态和时间（每分钟更新一次，减少闪烁）
      static unsigned long lastTimeUpdate = 0;
      if (now - lastTimeUpdate >= 60000) { // 每分钟更新一次
        int cm = sessionMillis / 60000; // 当前阅读分钟
        int tm = totalReadingSeconds / 60;
        int th = tm / 60;
        tm = tm % 60;
        String timeMsg = "Reading: " + String(cm) + "m\n";
        timeMsg += "Total: " + String(th) + "h " + String(tm) + "m\n";
        timeMsg += "Pomodoro: " + String(adaptivePomodoroMillis / 60000) + "m";
        showOnEInkSmart(timeMsg, PAGE_READING);
        lastTimeUpdate = now;
      }
    }
  }
  delay(1000);
}
