#include <Wire.h>
#include <Adafruit_PN532.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// === OLED 配置 ===
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// === PN532 配置 ===
#define SDA_PIN 8
#define SCL_PIN 9
Adafruit_PN532 nfc(SDA_PIN, SCL_PIN);

void setup() {
  Serial.begin(115200);
  delay(500);

  // === 初始化 I2C 总线 ===
  Wire.begin(SDA_PIN, SCL_PIN);

  // === 初始化 OLED ===
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED init failed!");
    for (;;)
      ;
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("OLED OK!");
  display.display();
  delay(500);

  // === 初始化 PN532 ===
  Serial.println("Initializing NFC...");
  nfc.begin();

  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("Didn't find PN532 board");
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("PN532 not found!");
    display.display();
    while (1)
      ;
  }

  Serial.print("Found PN532, FW ver: ");
  Serial.print((versiondata >> 16) & 0xFF, DEC);
  Serial.print('.');
  Serial.println((versiondata >> 8) & 0xFF, DEC);

  nfc.SAMConfig();
  Serial.println("Waiting for NFC card...");
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Waiting for card...");
  display.display();
}

void loop() {
  uint8_t uid[7];
  uint8_t uidLength;

  // === 检测 NFC 卡片 ===
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength)) {
    Serial.print("Found an NFC card with UID: ");

    // === 构造 UID 字符串 ===
    String uidStr = "";
    for (uint8_t i = 0; i < uidLength; i++) {
      Serial.print(uid[i], HEX);
      Serial.print(" ");
      if (uid[i] < 0x10) uidStr += "0";  // 补零
      uidStr += String(uid[i], HEX);
      if (i < uidLength - 1) uidStr += " ";
    }
    Serial.println();

    // === 在 OLED 上显示 UID ===
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Card Detected!");
    display.println("UID:");
    display.println(uidStr);
    display.display();

    delay(2000);  // 停留2秒
    // 恢复等待状态提示
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Waiting for card...");
    display.display();
  }
}
