#define NFC_INTERFACE_SPI

#include <SPI.h>
#include <PN532_SPI.h>
#include <PN532.h>

// 为 ESP32-C3 自定义 SPI 引脚（请根据你的连接修改）
#define PN532_SCK  4    // SPI Clock
#define PN532_MOSI 6    // SPI MOSI
#define PN532_MISO 5    // SPI MISO
#define PN532_SS   7    // SPI Chip Select (SDA)

// 使用Seeed-Studio/PN532库的初始化方式
PN532_SPI pn532spi(SPI, PN532_SS);
PN532 nfc(pn532spi);

void setup(void) {
  Serial.begin(115200);
  delay(1000);

  // 初始化 SPI
  SPI.begin(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);
  
  nfc.begin();
  
  // 获取固件版本
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("未找到PN532芯片");
    while (1);
  }
  
  Serial.print("找到PN532芯片，固件版本: 0x");
  Serial.println(versiondata, HEX);
  
  // 配置PN532
  nfc.SAMConfig();
  
  Serial.println("NFC 初始化完成，请靠近卡片...");
}

void loop(void) {
  uint8_t success;
  uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };
  uint8_t uidLength;
  
  // 等待ISO14443A类型的卡片
  success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength);
  
  if (success) {
    Serial.println("发现卡片!");
    Serial.print("UID长度: ");
    Serial.println(uidLength, DEC);
    Serial.print("UID: ");
    for (uint8_t i = 0; i < uidLength; i++) {
      Serial.print(uid[i], HEX);
      Serial.print(" ");
    }
    Serial.println();
    
    // 根据UID长度判断卡片类型
    if (uidLength == 4) {
      Serial.println("卡片类型: Mifare Classic");
    } else if (uidLength == 7) {
      Serial.println("卡片类型: Mifare Ultralight");
    } else {
      Serial.println("卡片类型: 未知");
    }
    
    // 等待卡片移开
    while (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength)) {
      delay(100);
    }
    Serial.println("卡片已移开");
  }
  
  delay(1000);
}
