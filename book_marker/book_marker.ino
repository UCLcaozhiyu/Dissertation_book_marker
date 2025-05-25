#include <Arduino.h>
#include <Preferences.h>

const int ldrPin = 0;
const int buzzerPin = 10;
const int lightThreshold = 1000;   // 光照不足阈值
const int shadowThreshold = 50;    // 极暗判定阈值（夹书用）
const unsigned long pomodoroDuration = 25 * 60; // 25分钟
const unsigned long breakDuration = 5 * 60;     // 5分钟

Preferences prefs;

// 状态追踪
bool isReading = false;
bool isOnBreak = false;
unsigned long startReadingMillis = 0;
unsigned long breakStartMillis = 0;
unsigned long sessionReadingSeconds = 0;
unsigned long totalReadingSeconds = 0;
unsigned long lastLightValue = 4095;
bool isInShadowState = false;
unsigned long shadowStartMillis = 0;

void beepShort() {
  digitalWrite(buzzerPin, LOW);
  delay(100);
  digitalWrite(buzzerPin, HIGH);
}

void beepLong() {
  for (int i = 0; i < 5; i++) {
    digitalWrite(buzzerPin, LOW);
    delay(200);
    digitalWrite(buzzerPin, HIGH);
    delay(200);
  }
}

// 格式化输出时间
void printTime(const char* label, unsigned long seconds) {
  unsigned int mins = seconds / 60;
  unsigned int secs = seconds % 60;
  Serial.print(label);
  Serial.print(mins);
  Serial.print(" 分 ");
  Serial.print(secs);
  Serial.println(" 秒");
}

void setup() {
  Serial.begin(115200);
  pinMode(ldrPin, INPUT);
  pinMode(buzzerPin, OUTPUT);
  digitalWrite(buzzerPin, HIGH);

  // 初始化存储并读取累计时间
  prefs.begin("reading", false);
  totalReadingSeconds = prefs.getULong("totalTime", 0);

  Serial.println("📚 阅读监测系统已启动");
  printTime("累计阅读时间：", totalReadingSeconds);
}

void loop() {
  int lightValue = analogRead(ldrPin);
  Serial.print("📡 当前光照值：");
  Serial.println(lightValue);

  unsigned long now = millis() / 1000;

  // 特殊处理：书签被夹入（极暗 + 快速突变）
  if (!isInShadowState && lightValue < shadowThreshold && lastLightValue > lightThreshold) {
    Serial.println("📘 检测到书签可能被夹入书中，提示10秒…");
    isInShadowState = true;
    shadowStartMillis = millis();
    beepLong();
  }

  if (isInShadowState) {
    if (millis() - shadowStartMillis > 10000) {
      Serial.println("📘 夹入提示结束");
      isInShadowState = false;
    }
    delay(500);
    return; // 暂停其他逻辑
  }

  lastLightValue = lightValue;

  // 光照过暗时结束阅读
  if (lightValue < lightThreshold && isReading) {
    isReading = false;
    unsigned long duration = now - startReadingMillis;
    sessionReadingSeconds += duration;
    totalReadingSeconds += duration;
    prefs.putULong("totalTime", totalReadingSeconds);

    Serial.println("🔕 光线不足，阅读暂停");
    printTime("本次阅读时间：", sessionReadingSeconds);
    printTime("累计阅读时间：", totalReadingSeconds);
    beepShort();
  }

  // 光照正常时处理阅读状态
  if (lightValue >= lightThreshold && !isReading && !isOnBreak) {
    isReading = true;
    startReadingMillis = now;
    Serial.println("✅ 光线良好，开始阅读...");
  }

  // 阅读过程中判断是否超时（番茄工作法）
  if (isReading) {
    unsigned long elapsed = now - startReadingMillis + sessionReadingSeconds;

    printTime("本次阅读累计：", elapsed);
    printTime("总阅读累计：", totalReadingSeconds + (elapsed - sessionReadingSeconds));

    if (elapsed >= pomodoroDuration) {
      Serial.println("🍅 阅读时间已达25分钟，开始休息");
      isReading = false;
      isOnBreak = true;
      breakStartMillis = now;
      beepLong();
    }
  }

  // 休息结束处理
  if (isOnBreak && (now - breakStartMillis >= breakDuration)) {
    Serial.println("🔔 休息结束，可以继续阅读！");
    isOnBreak = false;
    beepShort();
  }

  delay(1000);
}
