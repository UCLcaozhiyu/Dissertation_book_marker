/*
 * ESP32-WROOM-32D 深度睡眠功耗测试代码
 * 
 * 功能说明：
 * 1. 每次运行10秒后进入深度睡眠
 * 2. 深度睡眠20秒后自动唤醒
 * 3. 可通过GPIO33按钮提前唤醒
 * 4. 显示唤醒原因和运行次数
 * 
 * 测试功耗时的注意事项：
 * - 使用外部电源供电（不要用USB供电测试）
 * - 在3.3V电源线上串联电流表测量
 * - 确保WiFi和蓝牙已关闭
 * - 移除不必要的外围电路
 */

#include <WiFi.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <driver/rtc_io.h>

// 定时器唤醒配置
#define uS_TO_S_FACTOR 1000000ULL  /* 微秒到秒的转换因子 */
#define TIME_TO_SLEEP  20          /* ESP32深度睡眠时间（秒）*/
#define TIME_AWAKE     10          /* ESP32保持唤醒时间（秒）*/

// 外部唤醒配置（使用GPIO33作为唤醒按钮）
#define WAKEUP_GPIO GPIO_NUM_33
#define BUTTON_PIN_BITMASK 0x200000000  // 2^33 in hex

// RTC内存中的变量，深度睡眠后保持数值
RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR int wakeupCount = 0;
RTC_DATA_ATTR struct timeval sleep_enter_time;

// LED指示灯（可选，用于观察状态）
#define LED_PIN 2  // 使用内置LED

void print_wakeup_reason() {
  esp_sleep_wakeup_cause_t wakeup_reason;
  wakeup_reason = esp_sleep_get_wakeup_cause();

  Serial.print("Wakeup reason: ");
  switch(wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0:
      Serial.println("External signal using RTC_IO");
      break;
    case ESP_SLEEP_WAKEUP_EXT1:
      Serial.println("External signal using RTC_CNTL");
      break;
    case ESP_SLEEP_WAKEUP_TIMER:
      Serial.println("Timer");
      break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
      Serial.println("Touchpad");
      break;
    case ESP_SLEEP_WAKEUP_ULP:
      Serial.println("ULP program");
      break;
    default:
      Serial.printf("Not caused by deep sleep: %d\n", wakeup_reason);
      break;
  }
}

void print_sleep_time() {
  struct timeval now;
  gettimeofday(&now, NULL);
  int sleep_time_ms = (now.tv_sec - sleep_enter_time.tv_sec) * 1000 + (now.tv_usec - sleep_enter_time.tv_usec) / 1000;
  
  if (sleep_time_ms > 0) {
    Serial.printf("Deep sleep duration: %d ms\n", sleep_time_ms);
  }
}

void disable_wifi_bt() {
  // 关闭WiFi
  WiFi.mode(WIFI_OFF);
  btStop();
  
  Serial.println("WiFi and Bluetooth disabled");
}

void go_to_deep_sleep() {
  Serial.println("Preparing to enter deep sleep...");
  
  // 关闭LED
  digitalWrite(LED_PIN, LOW);
  
  // 记录进入睡眠的时间
  gettimeofday(&sleep_enter_time, NULL);
  
  // 配置定时器唤醒源
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
  Serial.println("Timer wakeup configured: " + String(TIME_TO_SLEEP) + " seconds");
  
  // 配置外部唤醒源（GPIO33，高电平唤醒）
  // 注意：确保GPIO33有下拉电阻，避免浮空
  esp_sleep_enable_ext0_wakeup(WAKEUP_GPIO, 1);  // 1 = 高电平唤醒
  Serial.println("External wakeup configured: GPIO" + String(WAKEUP_GPIO));
  
  // 如果需要测试ext1（多个GPIO唤醒），使用下面的代码替代ext0
  // esp_sleep_enable_ext1_wakeup(BUTTON_PIN_BITMASK, ESP_EXT1_WAKEUP_ANY_HIGH);
  
  
  // 配置GPIO下拉，确保稳定
  rtc_gpio_pullup_dis(WAKEUP_GPIO);
  rtc_gpio_pulldown_en(WAKEUP_GPIO);
  
  // 可选：关闭不需要的电源域以进一步降低功耗
  // esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
  // esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_OFF);
  // esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
  
  Serial.println("Entering deep sleep now...\n");
  Serial.flush();
  
  // 进入深度睡眠
  esp_deep_sleep_start();
  
  // 以下代码永远不会执行
  Serial.println("This will never be printed");
}

void setup() {
  Serial.begin(115200);
  delay(1000);  // 等待串口稳定
  
  // 增加启动计数
  ++bootCount;
  Serial.println("\n====================================");
  Serial.println("ESP32-WROOM-32D Deep Sleep Power Test");
  Serial.println("====================================");
  Serial.println("Boot count: " + String(bootCount));
  
  // 打印唤醒原因
  print_wakeup_reason();
  
  // 打印睡眠时长
  print_sleep_time();
  
  // 检测是否是深度睡眠唤醒
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED) {
    ++wakeupCount;
    Serial.println("Deep sleep wakeup count: " + String(wakeupCount));
  }
  
  // 设置LED引脚
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);  // 点亮LED表示系统运行中
  
  // 设置唤醒按钮引脚（如果需要读取按钮状态）
  pinMode(WAKEUP_GPIO, INPUT);
  
  // 关闭WiFi和蓝牙以降低功耗
  disable_wifi_bt();
  
  Serial.println("\nSystem will enter deep sleep in " + String(TIME_AWAKE) + " seconds");
  Serial.println("Or press GPIO" + String(WAKEUP_GPIO) + " button to wake up early\n");
  
  // 打印一些测试信息
  Serial.println("Test information:");
  Serial.printf("CPU frequency: %d MHz\n", getCpuFrequencyMhz());
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("Chip model: %s\n", ESP.getChipModel());
  Serial.printf("Chip revision: %d\n", ESP.getChipRevision());
  Serial.println("====================================\n");
}

void loop() {
  static unsigned long startTime = millis();
  static int countdown = TIME_AWAKE;
  
  // 每秒打印一次倒计时
  if (millis() - startTime >= 1000) {
    startTime = millis();
    Serial.println("Countdown: " + String(countdown) + " seconds");
    countdown--;
    
    // LED闪烁表示系统运行中
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }
  
  // 时间到，进入深度睡眠
  if (countdown < 0) {
    go_to_deep_sleep();
  }
  
  // 可以在这里添加其他测试代码
  // 例如：读取传感器、执行计算等
  delay(100);
}

/* 
 * 功耗测试步骤：
 * 
 * 1. 硬件准备：
 *    - 使用外部3.3V电源供电（不要用USB）
 *    - 在电源线上串联电流表
 *    - GPIO33连接按钮和10K下拉电阻
 *    - 移除不必要的LED和其他外围电路
 * 
 * 2. 测试方法：
 *    - 上传代码后断开USB
 *    - 使用电流表测量不同状态下的电流
 *    - 记录运行时电流（约40-80mA）
 *    - 记录深度睡眠电流（应该<10μA）
 * 
 * 3. 优化建议：
 *    - 使用低功耗的LDO稳压器
 *    - 确保所有未使用的GPIO设置为输入并启用下拉
 *    - 移除开发板上的电源LED和USB转串口芯片
 *    - 使用更低的CPU频率可进一步降低运行功耗
 * 
 * 4. 预期结果：
 *    - 活动模式：40-80mA（取决于CPU频率和外设）
 *    - 深度睡眠：5-10μA（原始芯片）
 *    - 注意：开发板可能因为额外电路导致睡眠电流较高（20-100μA）
 */