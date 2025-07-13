#include <SPI.h>
#include <MFRC522.h>

// 绑定引脚：使用 C3 Super Mini 上的实际 GPIO
#define SS_PIN    7    // RC522 的 SDA 接 GPIO7
#define RST_PIN   10   // RC522 的 RST 接 GPIO10

MFRC522 mfrc522(SS_PIN, RST_PIN);

void setup() {
  Serial.begin(115200);
  delay(2000); // 等串口准备好

  Serial.println("启动中...");

  SPI.begin(4, 5, 6);  // SCK=4, MISO=5, MOSI=6
  Serial.println("SPI 初始化完成");

  mfrc522.PCD_Init();
  Serial.println("RC522 初始化完成，请靠近卡片...");
}


void loop() {
  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial()) return;

  Serial.print("UID: ");
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    Serial.print(mfrc522.uid.uidByte[i] < 0x10 ? " 0" : " ");
    Serial.print(mfrc522.uid.uidByte[i], HEX);
  }
  Serial.println();
}