#include <Wire.h>
#include <Adafruit_PN532.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <SPI.h>

// NFC I2C引脚
#define SDA_PIN 8
#define SCL_PIN 9

// e-paper SPI引脚
#define EPD_CS   10
#define EPD_DC   4
#define EPD_RST  5
#define EPD_BUSY 3
#define EPD_MOSI 6
#define EPD_SCK  7

Adafruit_PN532 nfc(SDA_PIN, SCL_PIN);
GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> display(GxEPD2_154_D67(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// 显示函数
void showOnEInk(String msg) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(0, 30);
    // 支持多行
    int16_t y = 30;
    int16_t lineHeight = 16;
    int16_t start = 0, end = 0;
    while ((end = msg.indexOf('\n', start)) != -1) {
      display.setCursor(0, y);
      display.print(msg.substring(start, end));
      y += lineHeight;
      start = end + 1;
    }
    display.setCursor(0, y);
    display.print(msg.substring(start));
  } while (display.nextPage());
}

void setup(void) {
  Serial.begin(115200);

  // 初始化e-paper
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init();
  display.setRotation(1);

  Serial.println("Initializing NFC...");
  showOnEInk("Initializing NFC...");

  Wire.begin(SDA_PIN, SCL_PIN); // 启动 I2C
  nfc.begin();

  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("Didn't find PN532 board");
    showOnEInk("Didn't find\nPN532 board");
    while (1);
  }

  String chipInfo = "Found chip PN532,\nFirmware: " + String((versiondata >> 16) & 0xFF, DEC) + "." + String((versiondata >> 8) & 0xFF, DEC);
  Serial.println(chipInfo);
  showOnEInk(chipInfo);

  nfc.SAMConfig();
  Serial.println("Waiting for an NFC card...");
  showOnEInk("Waiting for an NFC card...");
}

void loop(void) {
  uint8_t uid[] = {0, 0, 0, 0, 0, 0, 0};
  uint8_t uidLength;

  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength)) {
    String uidMsg = "Found NFC card:\n";
    for (uint8_t i = 0; i < uidLength; i++) {
      uidMsg += String(uid[i], HEX);
      uidMsg += " ";
    }
    Serial.println(uidMsg);
    showOnEInk(uidMsg);
    delay(1000);
  }
}