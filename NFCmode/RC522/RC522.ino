#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1 // 通常不用
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// 绑定引脚：使用 C3 Super Mini 上的实际 GPIO
#define SS_PIN    7    // RC522 的 SDA 接 GPIO7
#define RST_PIN   10   // RC522 的 RST 接 GPIO10

MFRC522 mfrc522(SS_PIN, RST_PIN);

void setup() {
  Serial.begin(115200);
  delay(2000); // 等串口准备好

  // OLED I2C 引脚初始化
  Wire.begin(9, 8); // SDA=9, SCL=8

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { // 0x3C 是常见 I2C 地址
    Serial.println(F("OLED 初始化失败"));
    for(;;);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  display.println("OLED ");
  display.display();

  Serial.println("...");

  SPI.begin(4, 5, 6);  // SCK=4, MISO=5, MOSI=6
  Serial.println("SPI ");

  mfrc522.PCD_Init();
  Serial.println("RC522...");
}


void loop() {
  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial()) return;

  Serial.print("UID: ");
  display.clearDisplay();
  display.setCursor(0,0);
  display.print("UID:");

  for (byte i = 0; i < mfrc522.uid.size; i++) {
    Serial.print(mfrc522.uid.uidByte[i] < 0x10 ? " 0" : " ");
    Serial.print(mfrc522.uid.uidByte[i], HEX);

    display.print(mfrc522.uid.uidByte[i] < 0x10 ? " 0" : " ");
    display.print(mfrc522.uid.uidByte[i], HEX);
  }
  Serial.println();
  display.display();
}