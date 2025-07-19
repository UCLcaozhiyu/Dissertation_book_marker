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

const int ldrPin = 0;
const int piezoPin = 5;

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

#define SS_PIN    7
#define RST_PIN   10
MFRC522 mfrc522(SS_PIN, RST_PIN);
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

void drawBookAnimation(int frame) {
  int baseX = 110, baseY = 10;
  switch (frame % 3) {
    case 0:
      display.fillRect(baseX, baseY, 6, 10, SSD1306_WHITE); break;
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
  drawBookAnimation(animationFrame++);
  display.display();
}

void enterDeepSleep() {
  unsigned long waitStart = millis();
  while (millis() - waitStart < 2000) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      String uid = "";
      for (byte i = 0; i < mfrc522.uid.size; i++) {
        if (mfrc522.uid.uidByte[i] < 0x10) uid += "0";
        uid += String(mfrc522.uid.uidByte[i], HEX);
      }
      lastBookInfo = uid;
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
  pinMode(ldrPin, INPUT);
  pinMode(piezoPin, OUTPUT);
  digitalWrite(piezoPin, LOW);

  SPI.begin(4, 5, 6);
  mfrc522.PCD_Init();

  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
    justWokeUp = true;
  }

  turnOnOLED();
  prefs.begin("reading", false);
  totalReadingSeconds = prefs.getULong("totalSecs", 0);
  adaptivePomodoroMillis = prefs.getULong("pomodoro", defaultPomodoroMin * 60000UL);
  adaptiveTotalSessionTime = prefs.getULong("adaptTime", 0);
  adaptiveSessionCount = prefs.getULong("adaptCount", 0);
  lastBookInfo = prefs.getString("lastBook", "");

  if (justWokeUp && lastBookInfo != "") {
    drawRFIDInfo();
    delay(5000);
  }
}

void loop() {
  if (rfidDetected) {
    drawRFIDInfo();
    if (millis() - rfidDisplayStart > 5000) rfidDetected = false;
    delay(100);
    return;
  }

  int lightValue = analogRead(ldrPin);
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
      displayStatus("Paused", "Waiting light...");
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
      sessionStartMillis = now;
      isPaused = false;
      isReading = true;
      displayStatus("Good light", "Start reading");
    }
  } else {
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
      int cm = sessionMillis / 60000;
      int cs = (sessionMillis / 1000) % 60;
      int tm = totalReadingSeconds / 60;
      int th = tm / 60;
      tm = tm % 60;
      int ts = totalReadingSeconds % 60;
      display.clearDisplay();
      display.setCursor(0, 0);
      display.setTextSize(1);
      display.print("Reading: "); display.print(cm); display.print("m "); display.print(cs); display.println("s");
      display.print("Total: "); display.print(th); display.print("h "); display.print(tm); display.print("m "); display.print(ts); display.println("s");
      display.print("Pomodoro: "); display.print(adaptivePomodoroMillis / 60000); display.println("m");
      drawBookAnimation(animationFrame++);
      display.display();
    }
  }
  delay(1000);
}
