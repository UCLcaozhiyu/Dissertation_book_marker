#include <Adafruit_NeoPixel.h>

#define FSR_PIN 36      // FSR406B 连接到 ESP32 GPIO36 (模拟输入)
#define LED_PIN 5       // WS2812B 数据引脚 (接 GPIO5)
#define NUM_LEDS 20     // WS2812B 灯珠数量
#define THRESHOLD 600   // 触发阈值

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

void setup() {
    Serial.begin(115200);  // 设置波特率
    pinMode(FSR_PIN, INPUT);
    
    strip.begin();
    strip.setBrightness(30);  
    strip.fill(strip.Color(0, 0, 0));
    strip.show(); //初始化灯带
}

void loop() {
    int sensorValue = getStableFSRValue();  // 读取 FSR406B 数据
    Serial.println(sensorValue);            // 输出到串口监视器
    
    if (sensorValue > THRESHOLD) {
        gradualTurnOff();
    } else {
        gradualLightUp();
    }

    delay(500);  // 缓冲
}

// 获取稳定的 FSR406B 传感器值
int getStableFSRValue() {
    int sum = 0;
    for (int i = 0; i < 6; i++) {
        sum += analogRead(FSR_PIN);
        delay(10);
    }
    return sum / 6;  
}


void gradualLightUp() {
    for (int i = 0; i < NUM_LEDS; i++) {
        strip.setPixelColor(i, strip.Color( 237, 228, 15));
        strip.show();
        delay(50);
    }
}

// 依次熄灭 WS2812B
void gradualTurnOff() {
    for (int i = NUM_LEDS - 1; i >= 0; i--) {
        strip.setPixelColor(i, strip.Color(0, 0, 0)); 
        strip.show();
        delay(50);
    }
}
