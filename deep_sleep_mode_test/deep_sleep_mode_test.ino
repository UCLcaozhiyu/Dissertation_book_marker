#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "esp_system.h"
#include "esp_sleep.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

const int ldrPin = 0;
const int piezoPin = 10;

const int lightThreshold = 1000;
const int bookmarkThreshold = 50;
const uint64_t uS_TO_S_FACTOR = 1000000ULL;
const int TIME_TO_SLEEP = 10;

// Pomodoro configuration
const unsigned long workDuration = 25 * 60 * 1000UL;
const unsigned long restDuration = 5 * 60 * 1000UL;

bool isReading = false;
bool isPaused = false;
bool inRest = false;
unsigned long sessionStartMillis = 0;
unsigned long accumulatedSessionMillis = 0;
unsigned long restStartMillis = 0;
unsigned long totalReadingSeconds = 0;

Preferences prefs;

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
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("OLED init failed"));
    return;
  }
  display.ssd1306_command(SSD1306_DISPLAYON);
  display.clearDisplay();
  display.display();
}

void displayMessage(const String &line1, const String &line2 = "", const String &line3 = "") {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(line1);
  if (line2 != "") display.println(line2);
  if (line3 != "") display.println(line3);
  display.display();
}

void beep(int duration = 100) {
  digitalWrite(piezoPin, HIGH);
  delay(duration);
  digitalWrite(piezoPin, LOW);
}

void setup() {
  Serial.begin(115200);
  pinMode(ldrPin, INPUT);
  pinMode(piezoPin, OUTPUT);
  digitalWrite(piezoPin, LOW);

  turnOnOLED();
  delay(1000);

  prefs.begin("reading", false);
  totalReadingSeconds = prefs.getULong("totalSecs", 0);
}

void loop() {
  int lightValue = analogRead(ldrPin);
  Serial.print("[Light Level]: ");
  Serial.println(lightValue);

  unsigned long now = millis();

  if (lightValue < bookmarkThreshold) {
    if (isReading || isPaused) {
      unsigned long sessionMillis = accumulatedSessionMillis;
      if (isReading) sessionMillis += now - sessionStartMillis;
      unsigned long sessionSeconds = sessionMillis / 1000;
      totalReadingSeconds += sessionSeconds;
      prefs.putULong("totalSecs", totalReadingSeconds);
    }
    delay(1000);
    turnOffOLED();
    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
    esp_deep_sleep_start();
  }
  else if (lightValue < lightThreshold) {
    if (isReading) {
      accumulatedSessionMillis += now - sessionStartMillis;
      isReading = false;
      isPaused = true;
    }
    displayMessage("Low light.", "Please read in light place.");
  }
  else {
    if (!isReading) {
      if (inRest) {
        if (now - restStartMillis >= restDuration) {
          inRest = false;
          displayMessage("Rest done.", "Back to reading");
          beep(300);
          sessionStartMillis = now;
          accumulatedSessionMillis = 0;
          isReading = true;
        } else {
          displayMessage("Resting...", "");
          delay(1000);
          return;
        }
      } else {
        sessionStartMillis = now;
        isPaused = false;
        isReading = true;
        accumulatedSessionMillis = 0;
        displayMessage("Good light.", "Start reading.");
      }
    } else {
      unsigned long sessionMillis = accumulatedSessionMillis + (now - sessionStartMillis);
      if (sessionMillis >= workDuration) {
        inRest = true;
        isReading = false;
        totalReadingSeconds += sessionMillis / 1000;
        prefs.putULong("totalSecs", totalReadingSeconds);
        displayMessage("Pomodoro done!", "Take a break");
        beep(500);
        restStartMillis = now;
      } else {
        int cm = sessionMillis / 60000;
        int cs = (sessionMillis / 1000) % 60;
        int tm = totalReadingSeconds / 60;
        int ts = totalReadingSeconds % 60;
        displayMessage("Reading: " + String(cm) + "m " + String(cs) + "s", "Total: " + String(tm) + "m " + String(ts) + "s");
      }
    }
  }
  delay(1000);
}
