#include <Wire.h>
#include <Adafruit_PN532.h>

#define SDA_PIN 8
#define SCL_PIN 9

Adafruit_PN532 nfc(SDA_PIN, SCL_PIN); 

void setup(void) {
    Serial.begin(115200);
    Serial.println("Initializing NFC...");

    Wire.begin(SDA_PIN, SCL_PIN); // 启动 I2C
    nfc.begin();

    uint32_t versiondata = nfc.getFirmwareVersion();
    if (!versiondata) {
        Serial.println("Didn't find PN532 board");
        while (1);
    }

    Serial.print("Found chip PN532, Firmware version: ");
    Serial.print((versiondata >> 16) & 0xFF, DEC);
    Serial.print('.');
    Serial.println((versiondata >> 8) & 0xFF, DEC);

    nfc.SAMConfig();
    Serial.println("Waiting for an NFC card...");
}

void loop(void) {
    uint8_t uid[] = {0, 0, 0, 0, 0, 0, 0};
    uint8_t uidLength;

    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength)) {
        Serial.print("Found an NFC card with UID: ");
        for (uint8_t i = 0; i < uidLength; i++) {
            Serial.print(uid[i], HEX);
            Serial.print(" ");
        }
        Serial.println("");
        delay(1000);
    }
}
