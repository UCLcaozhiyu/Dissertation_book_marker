#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "esp_system.h"
#include "esp_sleep.h"
#include <SPI.h>
#include <MFRC522.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// 引脚分配说明：
// ldrPin = 0 (LDR光敏电阻)
// piezoPin = 12 (蜂鸣器，已避开SPI)
// OLED I2C: SDA=8, SCL=9
// PN532 SPI: SCK=4, MOSI=6, MISO=5, SS=7
// 改为RC522引脚
#define SS_PIN    7    // RC522 的 SDA 接 GPIO7
#define RST_PIN   10   // RC522 的 RST 接 GPIO10
MFRC522 mfrc522(SS_PIN, RST_PIN);
//
// 注意：如需更换开发板或引脚，请同步修改上面定义
const int ldrPin = 0;
const int piezoPin = 12;

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
bool rfidDetected = false;
unsigned long rfidDisplayStart = 0;
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

// 恢复 justWokeUp 变量声明
bool justWokeUp = false;

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

void turnOffOLED() {
  display.clearDisplay();
  display.display();
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  delay(100);
  Wire.end();
  pinMode(8, INPUT_PULLUP);
  pinMode(9, INPUT_PULLUP);
}

void turnOnOLED() {
  Wire.begin(8, 9);
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) return;
  display.ssd1306_command(SSD1306_DISPLAYON);
  display.clearDisplay();
  display.display();
}

void drawBookIcon() {
  int x = 110, y = 10;
  display.drawRect(x, y, 12, 10, SSD1306_WHITE);
  display.drawLine(x + 6, y, x + 6, y + 10, SSD1306_WHITE);
  display.drawLine(x + 2, y + 2, x + 10, y + 2, SSD1306_WHITE);
}

void drawRFIDInfo() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("Book Info:");
  display.println(lastBookInfo);
  display.display();
}

void displayStatus(const String& line1, const String& line2 = "", const String& line3 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(line1);
  if (line2 != "") display.println(line2);
  if (line3 != "") display.println(line3);
  drawBookIcon();
  display.display();
}

void enterDeepSleep() {
  unsigned long waitStart = millis();
  while (millis() - waitStart < 2000) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      String uidStr = "";
      for (byte i = 0; i < mfrc522.uid.size; i++) {
        if (mfrc522.uid.uidByte[i] < 0x10) uidStr += "0";
        uidStr += String(mfrc522.uid.uidByte[i], HEX);
      }
      lastBookInfo = uidStr;
      prefs.putString("lastBook", lastBookInfo);
      break;
    }
    delay(100);
  }
  isSleeping = true;
  displayStatus("Bookmark in", "Saving & sleep...");
  turnOffOLED();
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
  delay(2000);
  Serial.println("串口正常启动");

  Wire.begin(8, 9); // 按你的连线
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("OLED 初始化失败"));
    return; // 不要死循环
  }
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  display.println("HELLO");
  display.display();
  Serial.println("OLED显示已初始化");
}
void loop() {
  Serial.println("loop running");
  delay(1000);
}
