/*
 * =======================================================================
 * ESP32C3 NFC智能阅读追踪器 v2.0
 * =======================================================================
 * 
 * 功能特色：
 * - NFC卡片识别不同书籍，分别记录阅读时间
 * - 光线传感器自动检测阅读状态
 * - 自适应番茄钟功能，根据阅读习惯调整时长
 * - 精美像素艺术OLED界面设计
 * - 数据持久化存储
 * - I2C总线冲突优化
 * 
 * 硬件连接：
 * ESP32C3 开发板:
 * ├── OLED SSD1306 (128x64)
 * │   ├── SDA → GPIO 8
 * │   ├── SCL → GPIO 9
 * │   ├── VCC → 3.3V
 * │   └── GND → GND
 * ├── PN532 NFC模块
 * │   ├── SDA → GPIO 8 (与OLED共享I2C总线)
 * │   ├── SCL → GPIO 9 (与OLED共享I2C总线)
 * │   ├── VCC → 3.3V
 * │   └── GND → GND
 * ├── 光敏电阻 (LDR)
 * │   ├── 一端 → GPIO 0 (ADC输入)
 * │   ├── 另一端 → 3.3V
 * │   └── 下拉电阻10kΩ → GND
 * └── 蜂鸣器模块
 *     ├── VCC/+ → 3.3V (红线)
 *     ├── GND/- → GND  (黑线)
 *     └── I/O/S → GPIO 5 (信号线)
 * 
 * 作者: AI Assistant
 * 更新日期: 2025-01-16
 * =======================================================================
 */

// ===== 库文件引入 =====
#include <Arduino.h>           // Arduino核心库
#include "driver/ledc.h"       // ESP32 LEDC PWM驱动 (蜂鸣器控制)
#include <Preferences.h>       // ESP32非易失性存储库
#include <Wire.h>              // I2C通信库
#include <Adafruit_GFX.h>      // Adafruit图形库
#include <Adafruit_SSD1306.h>  // OLED显示屏驱动
#include "esp_system.h"        // ESP32系统函数
#include "esp_sleep.h"         // ESP32深度睡眠功能
#include <Adafruit_PN532.h>    // PN532 NFC模块驱动
#include <math.h>              // 数学函数库 (用于波浪线动画)

// ===== 硬件引脚定义 =====
#define SCREEN_WIDTH 128       // OLED屏幕宽度 (像素)
#define SCREEN_HEIGHT 64       // OLED屏幕高度 (像素)
#define OLED_RESET -1          // OLED复位引脚 (-1表示使用软件复位)
#define SCREEN_ADDRESS 0x3C    // OLED的I2C地址

#define SDA_PIN 8              // I2C数据线引脚 (SDA)
#define SCL_PIN 9              // I2C时钟线引脚 (SCL)

const int ldrPin = 0;          // 光敏电阻连接的ADC引脚
const int piezoPin = 5;        // 蜂鸣器连接的PWM引脚

// ===== 传感器阈值配置 =====
const int lightThreshold = 1000;    // 光线阈值：高于此值认为在阅读
const int bookmarkThreshold = 50;   // 书签阈值：低于此值进入深度睡眠
const uint64_t uS_TO_S_FACTOR = 1000000ULL;  // 微秒到秒的转换因子
const int TIME_TO_SLEEP = 10;       // 深度睡眠时间 (秒)

// ===== 像素图标数据 (16x16像素，单色位图) =====
// 📚 书本图标 - 用于表示书籍和阅读
const unsigned char icon_book[] PROGMEM = {
  0x00, 0x00, 0x1F, 0xF8, 0x10, 0x08, 0x10, 0x08, 0x10, 0x08, 0x10, 0x08, 
  0x1F, 0xF8, 0x10, 0x08, 0x10, 0x08, 0x10, 0x08, 0x10, 0x08, 0x10, 0x08, 
  0x10, 0x08, 0x1F, 0xF8, 0x00, 0x00, 0x00, 0x00
};

// 🕐 时钟图标 - 用于表示时间和计时
const unsigned char icon_clock[] PROGMEM = {
  0x03, 0xC0, 0x0C, 0x30, 0x10, 0x08, 0x20, 0x04, 0x20, 0x04, 0x41, 0x82, 
  0x41, 0x82, 0x41, 0x02, 0x41, 0x02, 0x20, 0x04, 0x20, 0x04, 0x10, 0x08, 
  0x0C, 0x30, 0x03, 0xC0, 0x00, 0x00, 0x00, 0x00
};

// ❤️ 心形图标 - 用于表示喜爱和欢迎
const unsigned char icon_heart[] PROGMEM = {
  0x00, 0x00, 0x0E, 0x70, 0x1F, 0xF8, 0x3F, 0xFC, 0x7F, 0xFE, 0x7F, 0xFE, 
  0x3F, 0xFC, 0x1F, 0xF8, 0x0F, 0xF0, 0x07, 0xE0, 0x03, 0xC0, 0x01, 0x80, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// ⭐ 星星图标 - 用于装饰和成就感
const unsigned char icon_star[] PROGMEM = {
  0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x0D, 0xB0, 0x1F, 0xF8, 0x0F, 0xF0, 
  0x07, 0xE0, 0x0F, 0xF0, 0x1B, 0xD8, 0x31, 0x8C, 0x60, 0x06, 0x60, 0x06, 
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// 🔋 电池图标 - 用于表示电源状态和传感器
const unsigned char icon_battery[] PROGMEM = {
  0x00, 0x00, 0x3F, 0xF0, 0x20, 0x18, 0x20, 0x18, 0x27, 0x98, 0x27, 0x98, 
  0x27, 0x98, 0x27, 0x98, 0x27, 0x98, 0x27, 0x98, 0x20, 0x18, 0x20, 0x18, 
  0x3F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// ===== 硬件对象实例化 =====
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);  // OLED显示屏对象
Adafruit_PN532 nfc(SDA_PIN, SCL_PIN);  // PN532 NFC模块对象

// ===== NFC相关变量 =====
String lastUID = "";              // 上次检测到的NFC卡片UID
bool showLastUIDOnBoot = false;   // 启动时是否显示上次UID
bool nfcInitialized = false;      // NFC模块是否初始化成功
bool oledInitialized = false;     // OLED是否初始化成功
bool i2cBusy = false;             // I2C总线互斥标志 (防止冲突)

// ===== 番茄钟配置 =====
const int defaultPomodoroMin = 25;                              // 默认番茄钟时长 (分钟)
unsigned long adaptivePomodoroMillis = defaultPomodoroMin * 60000UL;  // 自适应番茄钟时长 (毫秒)
unsigned long adaptiveTotalSessionTime = 0;                     // 累计会话时间 (用于计算平均值)
unsigned long adaptiveSessionCount = 0;                         // 会话计数 (用于计算平均值)
const unsigned long restDuration = 5 * 60 * 1000UL;           // 休息时间 (5分钟)

// ===== 阅读状态变量 =====
bool isReading = false;           // 当前是否在阅读
bool isPaused = false;            // 是否暂停
bool inRest = false;              // 是否在休息时间
bool isSleeping = false;          // 是否在睡眠状态

// ===== 时间记录变量 =====
unsigned long sessionStartMillis = 0;        // 当前会话开始时间
unsigned long accumulatedSessionMillis = 0;  // 累积的会话时间
unsigned long restStartMillis = 0;           // 休息开始时间
unsigned long totalReadingSeconds = 0;       // 总阅读时间 (秒)

// ===== 数据存储对象 =====
Preferences prefs;  // ESP32非易失性存储对象，用于保存数据到Flash

// ===== 光线趋势分析变量 =====
const int trendSize = 30;        // 趋势数据数组大小
int lightTrend[trendSize] = {0}; // 光线趋势数据数组
int trendIndex = 0;              // 当前趋势数据索引
float bestLuxEMA = 0.0;          // 最佳光照度的指数移动平均值
const float emaAlpha = 0.05;     // EMA平滑系数 (值越小越平滑)
int animationFrame = 0;          // 动画帧计数器

/*
 * =======================================================================
 * 功能函数定义区域
 * =======================================================================
 */

/**
 * I2C设备扫描函数
 * 功能：扫描I2C总线上的所有设备，用于调试硬件连接
 * 输出：通过串口打印找到的设备地址
 */
void scanI2CDevices() {
  Serial.println("Scanning I2C devices...");
  int deviceCount = 0;
  
  // 遍历所有可能的I2C地址 (0x01 到 0x7F)
  for (byte address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    byte error = Wire.endTransmission();
    
    if (error == 0) {  // 如果设备响应成功
      Serial.print("I2C device found at address 0x");
      if (address < 16) Serial.print("0");  // 补零对齐
      Serial.print(address, HEX);
      Serial.println();
      deviceCount++;
    }
  }
  
  Serial.print("Total devices found: ");
  Serial.println(deviceCount);
}

/**
 * 绘制屏幕四角装饰边框
 * 功能：在OLED屏幕四个角落绘制L形装饰线条
 * 用途：统一的界面风格，增加视觉美感
 */
void drawCornerDecorations() {
  // 左上角 L形
  display.drawLine(0, 0, 8, 0, SSD1306_WHITE);    // 水平线
  display.drawLine(0, 0, 0, 8, SSD1306_WHITE);    // 垂直线
  
  // 右上角 L形
  display.drawLine(119, 0, 127, 0, SSD1306_WHITE); // 水平线
  display.drawLine(127, 0, 127, 8, SSD1306_WHITE); // 垂直线
  
  // 左下角 L形
  display.drawLine(0, 56, 8, 56, SSD1306_WHITE);   // 水平线
  display.drawLine(0, 56, 0, 63, SSD1306_WHITE);   // 垂直线
  
  // 右下角 L形
  display.drawLine(119, 63, 127, 63, SSD1306_WHITE); // 水平线
  display.drawLine(127, 55, 127, 63, SSD1306_WHITE); // 垂直线
}

/**
 * 绘制动态轮播图标
 * 参数：x, y - 图标位置坐标
 *       frame - 当前动画帧数
 * 功能：循环显示4种图标，每20帧切换一次，创造动态效果
 */
void drawAnimatedIcon(int x, int y, int frame) {
  // 图标数组：书本、心形、星星、时钟
  const unsigned char* icons[] = {icon_book, icon_heart, icon_star, icon_clock};
  int iconIndex = (frame / 20) % 4; // 每20帧切换图标 (20帧 ≈ 10秒)
  
  display.drawBitmap(x, y, icons[iconIndex], 16, 16, SSD1306_WHITE);
}

/**
 * 绘制进度条
 * 参数：x, y - 进度条左上角坐标
 *       width, height - 进度条尺寸
 *       progress - 当前进度值
 *       maxProgress - 最大进度值
 * 功能：显示番茄钟进度或其他进度信息
 */
void drawProgressBar(int x, int y, int width, int height, int progress, int maxProgress) {
  // 绘制进度条外框
  display.drawRect(x, y, width, height, SSD1306_WHITE);
  
  // 计算填充宽度
  int fillWidth = (progress * (width - 2)) / maxProgress;
  if (fillWidth > 0) {
    // 绘制进度填充 (内部留1像素边距)
    display.fillRect(x + 1, y + 1, fillWidth, height - 2, SSD1306_WHITE);
  }
}

/**
 * 初始化OLED显示屏
 * 返回：bool - 初始化是否成功
 * 功能：初始化OLED并显示启动动画
 */
bool initializeOLED() {
  // 尝试初始化OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("OLED initialization failed!");
    return false;
  }
  
  // === 启动动画第一屏 ===
  display.clearDisplay();
  
  // 绘制装饰边框
  drawCornerDecorations();
  
  // 显示标题
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(15, 2);
  display.println("Reading Tracker");
  
  // 中央三个图标展示系统功能
  display.drawBitmap(30, 20, icon_book, 16, 16, SSD1306_WHITE);   // 书本：阅读
  display.drawBitmap(50, 20, icon_heart, 16, 16, SSD1306_WHITE);  // 心形：喜爱
  display.drawBitmap(70, 20, icon_clock, 16, 16, SSD1306_WHITE);  // 时钟：计时
  
  // 版本信息
  display.setCursor(45, 45);
  display.println("v2.0");
  
  // 底部装饰点线
  for (int i = 0; i < SCREEN_WIDTH; i += 8) {
    display.drawPixel(i, 55, SSD1306_WHITE);
  }
  
  display.display();
  delay(2000);  // 显示2秒
  
  // === 启动动画第二屏 ===
  display.clearDisplay();
  display.setCursor(35, 20);
  display.println("Starting...");
  display.drawBitmap(56, 35, icon_star, 16, 16, SSD1306_WHITE);
  display.display();
  
  Serial.println("OLED initialized successfully");
  return true;
}

/**
 * 初始化NFC模块
 * 返回：bool - 初始化是否成功
 * 功能：检测PN532模块，获取固件版本，配置SAM
 */
bool initializeNFC() {
  Serial.println("Initializing NFC...");
  nfc.begin();
  
  // 获取固件版本信息
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("PN532 not found!");
    return false;
  }
  
  // 显示固件版本 (格式：主版本.次版本)
  Serial.print("Found PN532, FW ver: ");
  Serial.print((versiondata >> 16) & 0xFF, DEC);
  Serial.print('.');
  Serial.println((versiondata >> 8) & 0xFF, DEC);
  
  // 配置SAM (Security Access Module)
  nfc.SAMConfig();
  Serial.println("NFC initialized successfully");
  return true;
}

/**
 * 将UID字节数组转换为十六进制字符串
 * 参数：uid - UID字节数组
 *       uidLength - UID长度
 * 返回：String - 格式化的UID字符串
 */
String getUIDString(uint8_t* uid, uint8_t uidLength) {
  String uidStr = "";
  for (uint8_t i = 0; i < uidLength; i++) {
    if (uid[i] < 0x10) uidStr += "0";  // 补零对齐
    uidStr += String(uid[i], HEX);
    if (i < uidLength - 1) uidStr += "";  // 可选择是否添加空格分隔
  }
  uidStr.toUpperCase();  // 转换为大写
  return uidStr;
}

/**
 * 加载书籍数据
 * 参数：bookUID - 书籍的UID
 * 功能：从存储中读取指定书籍的阅读时间和名称
 * 注意：当前版本简化实现，可扩展为多书籍管理
 */
void loadBookData(String bookUID) {
  String key = "book_" + bookUID;
  // currentBookSeconds = prefs.getULong(key.c_str(), 0);
  
  // 尝试获取书名，如果没有则使用默认名称
  String nameKey = "name_" + bookUID;
  // currentBookName = prefs.getString(nameKey.c_str(), "Book " + bookUID.substring(0, 4));
}

/**
 * 保存书籍阅读数据
 * 参数：bookUID - 书籍UID
 *       seconds - 阅读秒数
 * 功能：将书籍阅读时间保存到非易失性存储
 */
void saveBookData(String bookUID, unsigned long seconds) {
  String key = "book_" + bookUID;
  prefs.putULong(key.c_str(), seconds);
  Serial.println("Saved book " + bookUID + ": " + String(seconds) + " seconds");
}

/**
 * 保存书籍名称
 * 参数：bookUID - 书籍UID
 *       bookName - 书籍名称
 * 功能：保存自定义书名到存储
 */
void saveBookName(String bookUID, String bookName) {
  String nameKey = "name_" + bookUID;
  prefs.putString(nameKey.c_str(), bookName);
}

/**
 * 关闭OLED显示屏
 * 功能：关闭显示，释放I2C总线，设置引脚为上拉输入模式
 * 用途：进入深度睡眠前的清理工作
 */
void turnOffOLED() {
  if (oledInitialized) {
    display.clearDisplay();
    display.display();
    display.ssd1306_command(SSD1306_DISPLAYOFF);  // 关闭显示
    delay(100);
  }
  Wire.end();  // 释放I2C总线
  
  // 设置I2C引脚为上拉输入，降低功耗
  pinMode(SDA_PIN, INPUT_PULLUP);
  pinMode(SCL_PIN, INPUT_PULLUP);
}

/**
 * 开启OLED显示屏
 * 功能：重新初始化I2C总线和OLED显示屏
 * 用途：从深度睡眠唤醒后恢复显示
 */
void turnOnOLED() {
  Wire.begin(SDA_PIN, SCL_PIN);  // 重新初始化I2C
  delay(100);  // 等待总线稳定
  oledInitialized = initializeOLED();
}

/**
 * 绘制光线趋势图界面 (修改版 - 添加护眼提示)
 * 功能：显示光线传感器数据趋势，用于暂停阅读时的界面
 * 特色：包含电池图标、趋势曲线、装饰元素，以及护眼提示
 */
void drawTrendGraph() {
  if (!oledInitialized || i2cBusy) return;  // 检查显示状态和I2C总线
  
  display.clearDisplay();
  
  // 绘制装饰边框
  drawCornerDecorations();
  
  // 标题和图标
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(20, 2);
  display.println("Light Trend");
  
  // 左上角电池图标 (表示传感器状态)
  display.drawBitmap(2, 2, icon_battery, 16, 16, SSD1306_WHITE);
  
  // 显示当前光照度
  display.setCursor(20, 12);
  display.println("Lux: " + String((int)bestLuxEMA));
  
  // 护眼提示信息 (新增)
  display.setCursor(2, 22);
  display.println("Please do not read");
  display.setCursor(2, 32);
  display.println("in the dark.");
  display.setCursor(2, 42);
  display.println("Look for light.");
  
  // 绘制光线趋势曲线 (调整位置以适应新文本)
  for (int i = 0; i < trendSize - 1; i++) {
    // 计算曲线上的点坐标 (调整Y轴范围)
    int x1 = i * (SCREEN_WIDTH / trendSize);
    int y1 = SCREEN_HEIGHT - 8 - map(lightTrend[(trendIndex + i) % trendSize], 0, 4095, 0, 10);
    int x2 = (i + 1) * (SCREEN_WIDTH / trendSize);
    int y2 = SCREEN_HEIGHT - 8 - map(lightTrend[(trendIndex + i + 1) % trendSize], 0, 4095, 0, 10);
    
    // 绘制趋势线段
    display.drawLine(x1, y1, x2, y2, SSD1306_WHITE);
  }
  
  // 底部装饰点线
  for (int i = 0; i < SCREEN_WIDTH; i += 4) {
    display.drawPixel(i, SCREEN_HEIGHT - 2, SSD1306_WHITE);
  }
  
  display.display();
}

/**
 * 蜂鸣器提示音函数 (恢复蜂鸣器功能)
 * 参数：duration - 鸣叫持续时间 (毫秒)
 * 功能：播放指定时长的提示音
 * 使用PWM控制蜂鸣器音调和音量
 */
void beep(int duration = 100) {
  ledcWrite(0, 127);        // 设置PWM占空比为50% (127/255)
  delay(duration);          // 持续指定时间
  ledcWrite(0, 0);          // 关闭蜂鸣器
  Serial.println("Beep! Duration: " + String(duration) + "ms");
}

/**
 * NFC检测成功提示音 (双重哔哔声)
 * 功能：播放两次短促的提示音，表示NFC卡片检测成功
 */
void beepNFCSuccess() {
  beep(150);      // 第一声
  delay(100);     // 间隔
  beep(150);      // 第二声
  Serial.println("NFC Success Sound");
}

/**
 * 番茄钟完成提示音 (三重哔哔声)
 * 功能：播放三次提示音，表示番茄钟时间到了
 */
void beepPomodoroComplete() {
  beep(200);      // 第一声
  delay(150);     // 间隔
  beep(200);      // 第二声
  delay(150);     // 间隔
  beep(300);      // 第三声（稍长）
  Serial.println("Pomodoro Complete Sound");
}

/**
 * 光线不足警告音 (长音)
 * 功能：播放较长的警告音，提醒用户光线不足
 */
void beepLightWarning() {
  beep(800);      // 长警告音
  Serial.println("Light Warning Sound");
}

/**
 * 保存自适应番茄钟数据
 * 参数：sessionMillis - 本次会话时长 (毫秒)
 * 功能：根据用户阅读习惯动态调整番茄钟时长
 * 算法：
 *   - 如果会话时长>=当前番茄钟时长：增加5分钟 (最多60分钟)
 *   - 否则：计算历史平均值，调整到5-60分钟范围内
 */
void saveAdaptivePomodoro(unsigned long sessionMillis) {
  if (sessionMillis < 5 * 60 * 1000UL) return;  // 忽略少于5分钟的会话
  
  if (sessionMillis >= adaptivePomodoroMillis) {
    // 用户能坚持完整番茄钟，增加5分钟
    adaptivePomodoroMillis += 5 * 60 * 1000UL;
    adaptivePomodoroMillis = min(adaptivePomodoroMillis, 60 * 60 * 1000UL);  // 最多60分钟
  } else {
    // 用户提前结束，根据历史平均值调整
    adaptiveTotalSessionTime += sessionMillis;
    adaptiveSessionCount++;
    unsigned long averageMillis = adaptiveTotalSessionTime / adaptiveSessionCount;
    adaptivePomodoroMillis = constrain(averageMillis, 5 * 60 * 1000UL, 60 * 60 * 1000UL);
  }
  
  // 保存到非易失性存储
  prefs.putULong("pomodoro", adaptivePomodoroMillis);
  prefs.putULong("adaptTime", adaptiveTotalSessionTime);
  prefs.putULong("adaptCount", adaptiveSessionCount);
  
  Serial.print("[Pomodoro Adapted]: ");
  Serial.print(adaptivePomodoroMillis / 60000);
  Serial.println(" min");
}

/**
 * 显示NFC卡片检测界面并保存数据 (修改版 - 添加书名、简介和页数)
 * 参数：uid - NFC卡片的UID字节数组
 *       uidLength - UID长度
 * 功能：显示精美的NFC检测界面，保存UID到存储，显示书名、简介和页数
 */
void showNFCUIDAndSave(uint8_t* uid, uint8_t uidLength) {
  // 将UID转换为字符串格式
  String uidStr = "";
  for (uint8_t i = 0; i < uidLength; i++) {
    if (uid[i] < 0x10) uidStr += "0";  // 补零
    uidStr += String(uid[i], HEX);
    if (i < uidLength - 1) uidStr += " ";  // 空格分隔
  }
  
  // 保存UID到变量和存储
  lastUID = uidStr;
  prefs.putString("lastUID", lastUID);
  
  Serial.println("NFC Card detected: " + uidStr);
  
  delay(100);  // 稍作延迟，让I2C总线稳定
  
  if (oledInitialized) {
    display.clearDisplay();
    
    // 绘制装饰边框
    drawCornerDecorations();
    
    // 左侧书本图标 (表示检测到书籍)
    display.drawBitmap(2, 2, icon_book, 16, 16, SSD1306_WHITE);
    
    // 显示检测信息
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(20, 2);
    display.println("Book Found!");
    
    // 显示书名
    display.setCursor(20, 12);
    display.println("my notebook");
    
    // 显示页数
    display.setCursor(20, 22);
    display.println("Pages: One hundred");
    
    // 显示简介（分行显示）
    display.setCursor(2, 32);
    display.println("Zhiyu Cao's dev notes:");
    display.setCursor(2, 42);
    display.println("Design ideas, sketches");
    display.setCursor(2, 52);
    display.println("and formulas inside.");
    
    // 右上角心形图标 (表示欢迎)
    display.drawBitmap(110, 2, icon_heart, 16, 16, SSD1306_WHITE);
    
    // 底部UID信息（简化显示）
    display.setCursor(85, 52);
    display.println("ID:" + uidStr.substring(0, 6));
    
    display.display();
  }
  
  Serial.println("NFC card detected with beep sound");
  delay(3000);  // 延长显示时间到3秒，让用户能看完简介
}

/**
 * 检查NFC卡片
 * 返回：bool - 是否检测到卡片
 * 功能：尝试读取NFC卡片，如果检测到新卡片则处理
 * 优化：使用短超时时间，避免阻塞主循环
 */
bool checkNFCCard() {
  if (!nfcInitialized) return false;
  
  uint8_t uid[7];
  uint8_t uidLength;
  
  // 尝试读取NFC卡片 (100ms超时)
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 100)) {
    String newUID = getUIDString(uid, uidLength);
    
    if (newUID != lastUID) {  // 检测到新卡片
      lastUID = newUID;
      Serial.println("Detected book: " + lastUID);
      
      // 播放NFC检测成功提示音
      beepNFCSuccess();
      
      return true;
    }
    return true; // 同一张卡片
  }
  
  return false;  // 没有检测到卡片
}

/*
 * =======================================================================
 * 主程序区域
 * =======================================================================
 */

/**
 * 系统初始化函数
 * 功能：初始化所有硬件、加载保存的数据、显示启动界面
 */
void setup() {
  // === 串口通信初始化 ===
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Reading Tracker Starting ===");
  
  // === GPIO引脚初始化 ===
  pinMode(ldrPin, INPUT);      // 光敏电阻为输入
  pinMode(piezoPin, OUTPUT);   // 蜂鸣器为输出
  digitalWrite(piezoPin, LOW); // 蜂鸣器初始为低电平
  
  // 蜂鸣器PWM初始化 (适配新版ESP32 Arduino Core)
  if (!ledcAttach(piezoPin, 3000, 8)) {        // GPIO5，3000Hz频率，8位分辨率
    Serial.println("Failed to attach LEDC to pin");
  }
  
  // === I2C总线初始化 ===
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(500);  // 等待I2C总线稳定
  
  // === I2C设备扫描 (调试用) ===
  scanI2CDevices();
  
  // === OLED显示屏初始化 ===
  oledInitialized = initializeOLED();
  if (!oledInitialized) {
    Serial.println("OLED failed, continuing without display...");
  }
  
  // === 数据存储初始化 ===
  prefs.begin("reading", false);  // 打开命名空间"reading"
  
  // 加载保存的数据
  totalReadingSeconds = prefs.getULong("totalSecs", 0);        // 总阅读时间
  adaptivePomodoroMillis = prefs.getULong("pomodoro", defaultPomodoroMin * 60000UL);  // 番茄钟时长
  adaptiveTotalSessionTime = prefs.getULong("adaptTime", 0);   // 累计会话时间
  adaptiveSessionCount = prefs.getULong("adaptCount", 0);      // 会话计数
  
  // === NFC模块初始化 ===
  nfcInitialized = initializeNFC();
  if (!nfcInitialized) {
    Serial.println("NFC failed, continuing without NFC...");
    if (oledInitialized) {
      display.clearDisplay();
      display.setCursor(0, 0);
      display.println("NFC Failed!");
      display.println("Check connections");
      display.display();
    }
  }
  
  // === 显示上次使用的书籍信息 (修改版 - 添加书名) ===
  lastUID = prefs.getString("lastUID", "");
  if (lastUID.length() > 0 && oledInitialized) {
    display.clearDisplay();
    
    // 绘制装饰边框
    drawCornerDecorations();
    
    // 欢迎回来页面设计
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(25, 2);
    display.println("Welcome Back!");
    
    // 左侧书本图标
    display.drawBitmap(6, 15, icon_book, 16, 16, SSD1306_WHITE);
    
    // 上次书籍信息
    display.setCursor(28, 15);
    display.println("Last Book:");
    display.setCursor(28, 25);
    display.println("my notebook");  // 新增：显示书名
    display.setCursor(28, 35);
    display.println(lastUID.substring(0, 8));  // 显示UID前8位
    
    // 右侧心形图标
    display.drawBitmap(106, 15, icon_heart, 16, 16, SSD1306_WHITE);
    
    // 底部装饰：三个星星 (位置稍作调整)
    for (int i = 0; i < 3; i++) {
      display.drawBitmap(30 + i * 25, 50, icon_star, 16, 16, SSD1306_WHITE);
    }
    
    display.display();
    delay(3000);  // 显示3秒
  }
  
  // === 初始化完成提示 ===
  Serial.println("=== Setup Complete ===");
  Serial.print("OLED: ");
  Serial.println(oledInitialized ? "OK" : "Failed");
  Serial.print("NFC: ");
  Serial.println(nfcInitialized ? "OK" : "Failed");
  
  // 播放启动完成提示音
  beep(200);
  delay(100);
  beep(200);
  Serial.println("System ready with buzzer enabled!");
}

/**
 * 主循环函数
 * 功能：持续监测光线、NFC卡片，管理阅读状态，显示相应界面
 */
void loop() {
  // === 读取光线传感器数据 ===
  int lightValue = analogRead(ldrPin);                    // 读取ADC值 (0-4095)
  lightTrend[trendIndex] = lightValue;                    // 保存到趋势数组
  trendIndex = (trendIndex + 1) % trendSize;              // 更新数组索引 (循环)

  // === 更新光照度EMA (指数移动平均) ===
  if (isReading) {
    bestLuxEMA = (1 - emaAlpha) * bestLuxEMA + emaAlpha * lightValue;
    bestLuxEMA = constrain(bestLuxEMA, 100, 3000);        // 限制在合理范围
  }

  unsigned long now = millis();  // 获取当前时间戳
  
  // === NFC卡片检测 (每3秒检测一次，避免I2C冲突) ===
  static unsigned long lastNFCCheck = 0;
  if (nfcInitialized && !i2cBusy && (now - lastNFCCheck > 3000)) {
    lastNFCCheck = now;
    i2cBusy = true;  // 锁定I2C总线，防止OLED和NFC同时访问
    
    uint8_t uid[7];
    uint8_t uidLength;
    
    // 尝试读取NFC卡片 (1秒超时)
    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 1000)) {
      showNFCUIDAndSave(uid, uidLength);  // 显示NFC检测界面
      i2cBusy = false;  // 释放I2C总线
      delay(2000);      // 显示完成后等待，避免连续读取
      return;
    }
    
    i2cBusy = false;  // 释放I2C总线
  }

  // === 检查是否进入深度睡眠模式 ===
  if (lightValue < bookmarkThreshold) {
    // 保存当前会话数据
    if (isReading || isPaused) {
      unsigned long sessionMillis = accumulatedSessionMillis;
      if (isReading) sessionMillis += now - sessionStartMillis;
      totalReadingSeconds += sessionMillis / 1000;
      
      saveAdaptivePomodoro(sessionMillis);  // 保存自适应番茄钟数据
      prefs.putULong("totalSecs", totalReadingSeconds);  // 保存总阅读时间
    }
    
    // 显示睡眠界面
    if (oledInitialized && !i2cBusy) {
      display.clearDisplay();
      
      // 绘制装饰边框
      drawCornerDecorations();
      
      // 睡眠提示标题
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(25, 2);
      display.println("Good Night!");
      
      // 左侧电池图标 (表示睡眠/低功耗状态)
      display.drawBitmap(6, 20, icon_battery, 16, 16, SSD1306_WHITE);
      
      // 睡眠倒计时信息
      display.setCursor(28, 20);
      display.println("Sleeping in");
      display.setCursor(35, 30);
      display.println(String(TIME_TO_SLEEP) + " seconds");
      
      // 右侧时钟图标 (表示时间)
      display.drawBitmap(106, 20, icon_clock, 16, 16, SSD1306_WHITE);
      
      // 底部睡眠装饰 "Z z z"
      display.setCursor(45, 50);
      display.setTextSize(2);
      display.println("Z z z");
      
      display.display();
      delay(1000);
    }
    
    // 进入深度睡眠
    turnOffOLED();
    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
    esp_deep_sleep_start();
  }
  
  // === 检查光线阈值 (暂停阅读) ===
  else if (lightValue < lightThreshold) {
    if (isReading) {
      // 从阅读状态转为暂停状态
      accumulatedSessionMillis += now - sessionStartMillis;
      isReading = false;
      isPaused = true;
    }
    drawTrendGraph();  // 显示光线趋势图 (包含护眼提示)
    beepLightWarning();        // 播放光线不足警告音
  }
  
  // === 正常阅读模式 ===
  else {
    if (!isReading) {
      if (inRest) {
        // === 休息时间逻辑 ===
        if (restStartMillis == 0) restStartMillis = now;  // 记录休息开始时间
        
        if (now - restStartMillis >= restDuration) {
          // 休息时间结束，开始新的阅读会话
          inRest = false;
          beep(300);                    // 播放短提示音 (休息结束)
          sessionStartMillis = now;     // 记录新会话开始时间
          accumulatedSessionMillis = 0; // 重置累积时间
          isReading = true;
        } else {
          // 显示休息界面
          if (oledInitialized && !i2cBusy) {
            display.clearDisplay();
            
            // 绘制装饰边框
            drawCornerDecorations();
            
            // 休息时间标题
            display.setTextSize(1);
            display.setTextColor(SSD1306_WHITE);
            display.setCursor(30, 2);
            display.println("Rest Time");
            
            // 左侧心形图标 (表示放松)
            display.drawBitmap(6, 15, icon_heart, 16, 16, SSD1306_WHITE);
            
            // 中央休息提示信息
            display.setTextSize(1);
            display.setCursor(28, 20);
            display.println("Take a break!");
            display.setCursor(35, 30);
            display.println("Relax...");
            
            // 右侧星星图标 (表示美好)
            display.drawBitmap(106, 15, icon_star, 16, 16, SSD1306_WHITE);
            
            // 计算并显示剩余休息时间
            unsigned long restRemaining = restDuration - (now - restStartMillis);
            int restMin = restRemaining / 60000;
            int restSec = (restRemaining / 1000) % 60;
            
            display.setCursor(35, 45);
            display.println(String(restMin) + ":" + (restSec < 10 ? "0" : "") + String(restSec));
            
            // 动态波浪线装饰 (使用sin函数创建波浪效果)
            for (int x = 0; x < SCREEN_WIDTH; x += 8) {
              int y = 55 + (sin(x * 0.3 + millis() * 0.01) * 3);
              display.drawPixel(x, y, SSD1306_WHITE);
              display.drawPixel(x + 2, y + 1, SSD1306_WHITE);
            }
            
            display.display();
          }
          delay(1000);
          return;  // 休息期间不执行后续逻辑
        }
      } else {
        // 从暂停状态恢复到阅读状态
        sessionStartMillis = now;
        isPaused = false;
        isReading = true;
      }
    } else {
      // === 阅读状态逻辑 ===
      unsigned long sessionMillis = accumulatedSessionMillis + (now - sessionStartMillis);
      
      if (sessionMillis >= adaptivePomodoroMillis) {
        // 番茄钟时间到，进入休息状态
        inRest = true;
        isReading = false;
        restStartMillis = 0;
        totalReadingSeconds += sessionMillis / 1000;
        
        saveAdaptivePomodoro(sessionMillis);              // 保存番茄钟数据
        prefs.putULong("totalSecs", totalReadingSeconds); // 保存总阅读时间
        beepPomodoroComplete();                           // 播放番茄钟完成提示音
      } else {
        // 显示阅读状态界面
        if (oledInitialized && !i2cBusy) {
          // 计算时间显示格式
          int cm = sessionMillis / 60000;                    // 当前会话分钟数
          int cs = (sessionMillis / 1000) % 60;              // 当前会话秒数
          unsigned long totalMinutes = totalReadingSeconds / 60;
          unsigned long totalHours = totalMinutes / 60;
          unsigned long totalMinsOnly = totalMinutes % 60;
          int pm = adaptivePomodoroMillis / 60000;           // 番茄钟目标分钟数
          
          // 格式化总时间字符串
          String totalStr = (totalHours > 0) ? 
            (String(totalHours) + "h " + String(totalMinsOnly) + "m") : 
            (String(totalMinsOnly) + "m");
          
          display.clearDisplay();
          
          // 绘制装饰边框
          drawCornerDecorations();
          
          // 阅读模式标题
          display.setTextSize(1);
          display.setTextColor(SSD1306_WHITE);
          display.setCursor(22, 2);
          display.println("Reading Mode");
          
          // 左侧时钟图标
          display.drawBitmap(2, 2, icon_clock, 16, 16, SSD1306_WHITE);
          
          // 当前会话时间 (大字体突出显示)
          display.setTextSize(2);
          display.setCursor(8, 15);
          display.print(String(cm) + ":" + (cs < 10 ? "0" : "") + String(cs));
          
          // 右侧动态轮播图标
          drawAnimatedIcon(108, 15, animationFrame);
          
          // 统计信息
          display.setTextSize(1);
          display.setCursor(8, 35);
          display.println("Total: " + totalStr);
          
          display.setCursor(8, 45);
          display.println("Target: " + String(pm) + " min");
          
          // 番茄钟进度条
          int progress = (sessionMillis * 100) / adaptivePomodoroMillis;
          drawProgressBar(8, 55, 112, 6, progress, 100);
          
          // NFC状态指示圆点 (实心=正常，空心=故障)
          display.fillCircle(120, 45, 2, nfcInitialized ? SSD1306_WHITE : SSD1306_BLACK);
          display.drawCircle(120, 45, 2, SSD1306_WHITE);
          
          display.display();
          animationFrame++;  // 更新动画帧计数
        }
      }
    }
  }
  
  delay(500);  // 主循环延迟，平衡响应速度和功耗
}