#include <Arduino.h>
#include <Preferences.h>
#include "esp_system.h"
#include "esp_sleep.h"
#include "driver/gpio.h"

// === 引脚定义 ===
const int ldrPin = 0;
const int buzzerPin = 10;

// === 参数定义 ===
const int lightThreshold = 1000;
const int bookmarkThreshold = 50;

// === 番茄工作法参数 ===
const unsigned long workDuration = 25 * 60 * 1000UL;
const unsigned long restDuration = 5 * 60 * 1000UL;

// === 状态变量 ===
bool isReading = false;
bool isPaused = false;
bool inRest = false;
unsigned long sessionStartMillis = 0;
unsigned long accumulatedSessionMillis = 0;
unsigned long restStartMillis = 0;
unsigned long lastBeepMillis = 0;
bool hasShownBookmarkHint = false;
bool hasBeepedLowLight = false;
bool isBeeping = false;
unsigned long beepStartMillis = 0;
bool isSleeping = false; // 新增标志位

// === 累计时间持久化 ===
unsigned long totalReadingSeconds = 0;
Preferences prefs;

void beep(int duration = 100) {
  digitalWrite(buzzerPin, LOW);
  delay(duration);
  digitalWrite(buzzerPin, HIGH);
}

void printReadingTime(unsigned long seconds, bool ended) {
  int mins = seconds / 60;
  int secs = seconds % 60;
  if (ended) {
    Serial.print("\n\U0001F9E0 累计阅读时间：");
  } else {
    Serial.print("⏱️ 阅读中，本次已读：");
  }
  Serial.print(mins);
  Serial.print(" 分 ");
  Serial.print(secs);
  Serial.println(" 秒");
}

void enterDeepSleep() {
  Serial.println("🛌 已放回书中，进入深度睡眠节能...");

  isBeeping = false;
  isSleeping = true;
  digitalWrite(buzzerPin, HIGH);
  Serial.println("🛑 Beep disabled for sleep");

  prefs.putULong("totalSecs", totalReadingSeconds);

  // 禁用蜂鸣器引脚避免漏电导致响声
  pinMode(buzzerPin, INPUT);

  gpio_wakeup_enable(GPIO_NUM_0, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  delay(100);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  pinMode(ldrPin, INPUT);
  pinMode(buzzerPin, OUTPUT);
  digitalWrite(buzzerPin, HIGH);

  prefs.begin("reading", false);
  totalReadingSeconds = prefs.getULong("totalSecs", 0);

  Serial.println("\n📚 阅读监测系统启动");
  printReadingTime(totalReadingSeconds, true);
}

void loop() {
  if (isSleeping) return;  // 睡眠状态不再继续运行逻辑

  int lightValue = analogRead(ldrPin);
  unsigned long now = millis();

  Serial.print("\n📡 当前光照值：");
  Serial.println(lightValue);

  if (isBeeping) {
    if (now - beepStartMillis <= 5000) {
      digitalWrite(buzzerPin, (now / 500) % 2 == 0 ? LOW : HIGH);
    } else {
      digitalWrite(buzzerPin, HIGH);
      isBeeping = false;
    }
  }

  if (lightValue < bookmarkThreshold) {
    if (!hasShownBookmarkHint) {
      Serial.println("📕 检测到书签可能已夹入书中，结束本次阅读并保存...");
      if (isReading || isPaused) {
        unsigned long sessionMillis = accumulatedSessionMillis;
        if (isReading) {
          sessionMillis += now - sessionStartMillis;
        }
        unsigned long sessionSeconds = sessionMillis / 1000;
        totalReadingSeconds += sessionSeconds;
        prefs.putULong("totalSecs", totalReadingSeconds);
        printReadingTime(totalReadingSeconds, true);
        isReading = false;
        isPaused = false;
        accumulatedSessionMillis = 0;
      }
      hasShownBookmarkHint = true;
      enterDeepSleep();
    }
    delay(1000);
    return;
  } else {
    hasShownBookmarkHint = false;
  }

  if (inRest) {
    if (now - restStartMillis >= restDuration) {
      Serial.println("✅ 休息结束，开始下一轮阅读");
      beep(300);
      inRest = false;
      sessionStartMillis = now;
      accumulatedSessionMillis = 0;
      isReading = true;
    } else {
      Serial.println("💤 正在休息...");
      delay(1000);
      return;
    }
  }

  if (lightValue >= lightThreshold) {
    hasBeepedLowLight = false;
    isBeeping = false;
    digitalWrite(buzzerPin, HIGH);

    if (!isReading) {
      if (isPaused) {
        sessionStartMillis = now;
        isReading = true;
        isPaused = false;
        Serial.println("🔄 光照恢复，继续阅读...");
      } else {
        sessionStartMillis = now;
        accumulatedSessionMillis = 0;
        isReading = true;
        Serial.println("✅ 光线良好，开始阅读...");
      }
    } else {
      unsigned long sessionMillis = accumulatedSessionMillis + (now - sessionStartMillis);
      unsigned long sessionSeconds = sessionMillis / 1000;
      printReadingTime(sessionSeconds, false);
      printReadingTime(totalReadingSeconds, true);

      if (sessionMillis >= workDuration) {
        Serial.println("🍅 番茄时间结束，请休息 5 分钟");
        beep(500);
        restStartMillis = now;
        inRest = true;
        isReading = false;

        totalReadingSeconds += sessionMillis / 1000;
        prefs.putULong("totalSecs", totalReadingSeconds);
        printReadingTime(totalReadingSeconds, true);
        accumulatedSessionMillis = 0;
      }
    }
  } else if (lightValue >= bookmarkThreshold && lightValue < lightThreshold) {
    if (isReading) {
      accumulatedSessionMillis += now - sessionStartMillis;
      Serial.println("🔅 光线不足，暂停阅读...");
      if (!hasBeepedLowLight) {
        beep();
        hasBeepedLowLight = true;
        isBeeping = true;
        beepStartMillis = now;
      }
      isReading = false;
      isPaused = true;
    } else {
      Serial.println("🔅 暂停中，等待光照恢复...");
    }
  }

  delay(1000);
}
