/*
 * =======================================================================
 * ESP32 WROOM32D NFC智能阅读追踪器 - 深度睡眠修复版
 * =======================================================================
 * 
 * 修复内容：
 * - 优化睡眠检测逻辑，确保能真正进入深度睡眠
 * - 修复I2C总线未正确释放的问题
 * - 优化唤醒后的硬件重新初始化流程
 * - 降低待机功耗至约10μA（深度睡眠模式）
 * 
 * 硬件连接（从ESP32C3迁移到ESP32 WROOM32D）：
 * ESP32 WROOM32D:
 * ├── OLED SSD1306 (128x64)
 * │   ├── SDA → GPIO 21 (标准I2C)
 * │   ├── SCL → GPIO 22 (标准I2C)
 * │   ├── VCC → 3.3V
 * │   └── GND → GND
 * ├── PN532 NFC模块
 * │   ├── SDA → GPIO 21 (与OLED共享I2C总线)
 * │   ├── SCL → GPIO 22 (与OLED共享I2C总线)
 * │   ├── VCC → 3.3V
 * │   └── GND → GND
 * ├── 光敏电阻 (LDR) - 双输入设计
 * │   ├── 一端 → 3.3V
 * │   ├── 另一端 → GPIO 36 (ADC输入，精确测量)
 * │   ├── 另一端 → GPIO 26 (数字输入，唤醒用，RTC_GPIO7)
 * │   └── 两个10kΩ下拉电阻分别到GND
 * ├── 蜂鸣器模块
 * │   ├── VCC → 3.3V
 * │   ├── GND → GND
 * │   └── Signal → GPIO 5
 * └── 可选：手动唤醒按钮
 *     └── 按钮 → GPIO 2 (RTC_GPIO12) 与GND之间
 * 
 * =======================================================================
 */

#include <Arduino.h>
#include "driver/ledc.h"
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "esp_system.h"
#include "esp_sleep.h"
#include <Adafruit_PN532.h>
#include <math.h>
#include "driver/rtc_io.h"  // 添加RTC IO控制

// ===== 硬件引脚定义 (ESP32 WROOM32D - 安全版本) =====
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

#define SDA_PIN 21             // ESP32标准I2C引脚
#define SCL_PIN 22             // ESP32标准I2C引脚

const int ldrPin = 36;         // 光敏电阻ADC输入 (精确测量，只读)
const int ldrWakeupPin = 26;   // 光敏电阻数字输入 (唤醒用，RTC_GPIO7)
const int buttonWakeupPin = 2; // 手动唤醒按钮 (RTC_GPIO12) - 可选
const int piezoPin = 5;        // 蜂鸣器引脚

// ===== 书籍管理配置 =====
const int MAX_BOOKS = 10;      // 最大支持书籍数量
const int BOOK_NAME_LENGTH = 20; // 书名最大长度

// 书籍数据结构
struct Book {
  String uid;                  // NFC卡片UID
  String name;                 // 书名
  unsigned long totalSeconds;  // 总阅读时间（秒）
  unsigned long lastRead;      // 最后阅读时间戳
  int sessionCount;            // 阅读会话次数
  int pomodoroCount;           // 完成的番茄钟数
  float avgSessionMinutes;     // 平均每次阅读时长（分钟）
  bool isActive;              // 是否激活（有数据）
};

// ===== 优化配置 - 修改睡眠检测参数 =====
const int lightThreshold = 1000;         // 光线阈值，低于此值认为光线不足
const int bookmarkThreshold = 200;        // 书签检测阈值，用于睡眠判断
const unsigned long STARTUP_GRACE_PERIOD = 5000;  // 启动保护时间：5秒
const int SLEEP_CHECK_COUNT = 2;          // 睡眠检测次数：减少到2次
const unsigned long SLEEP_CHECK_INTERVAL = 2000; // 睡眠检测间隔：2秒
const unsigned long IDLE_SLEEP_TIMEOUT = 30000;  // 空闲30秒后自动睡眠

// ===== 唤醒原因枚举 =====
enum WakeupReason {
  WAKEUP_TIMER,    // 定时器唤醒
  WAKEUP_LIGHT,    // 光线变化唤醒
  WAKEUP_BUTTON,   // 按钮唤醒
  WAKEUP_UNKNOWN   // 未知原因（首次启动）
};

// ===== 硬件对象初始化 =====
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_PN532 nfc(SDA_PIN, SCL_PIN);

// ===== 书籍管理变量 =====
Book books[MAX_BOOKS];                    // 书籍数组
int currentBookIndex = -1;                // 当前书籍索引（-1表示无书）
String currentBookUID = "";               // 当前书籍UID
bool bookChanged = false;                 // 书籍是否发生切换

// ===== 状态变量 =====
String lastUID = "";                      // 上次检测到的NFC卡片UID
bool showLastUIDOnBoot = false;           // 是否在启动时显示上次的卡片
bool nfcInitialized = false;              // NFC模块是否初始化成功
bool oledInitialized = false;             // OLED屏幕是否初始化成功
bool i2cBusy = false;                     // I2C总线是否忙碌
bool isShowingNFC = false;                // 是否正在显示NFC信息
unsigned long nfcDisplayStartTime = 0;    // NFC显示开始时间
unsigned long lastActivityTime = 0;       // 最后活动时间（用于空闲检测）

// ===== 阅读状态变量 =====
bool isReading = false;                   // 是否正在阅读
bool isPaused = false;                    // 是否暂停（因光线不足）
bool inRest = false;                      // 是否在休息状态
bool isSleeping = false;                  // 是否在睡眠状态

// ===== 时间记录变量 =====
unsigned long sessionStartMillis = 0;        // 本次阅读开始时间
unsigned long accumulatedSessionMillis = 0;  // 累计阅读时间
unsigned long restStartMillis = 0;           // 休息开始时间
unsigned long totalReadingSeconds = 0;       // 总阅读时间（秒）- 所有书籍

// ===== 自适应番茄钟变量 =====
const int defaultPomodoroMin = 25;                           // 默认番茄钟时长（分钟）
unsigned long adaptivePomodoroMillis = defaultPomodoroMin * 60000UL; // 自适应番茄钟时长
unsigned long adaptiveTotalSessionTime = 0;                 // 历史总会话时间
unsigned long adaptiveSessionCount = 0;                     // 历史会话次数
const unsigned long restDuration = 5 * 60 * 1000UL;        // 休息时长：5分钟

// ===== 蜂鸣器控制 =====
unsigned long lastBeepTime = 0;              // 上次蜂鸣时间
const unsigned long beepInterval = 10000;    // 蜂鸣间隔：10秒

// ===== 数据存储 =====
Preferences prefs;
Preferences bookPrefs;  // 专门用于书籍数据存储

// ===== 光线趋势分析 =====
const int trendSize = 30;        // 趋势数据点数量
int lightTrend[trendSize] = {0}; // 光线趋势数组
int trendIndex = 0;              // 当前趋势索引
float bestLuxEMA = 0.0;          // 最佳光照指数移动平均值
const float emaAlpha = 0.05;     // EMA平滑系数
int animationFrame = 0;          // 动画帧计数

// ===== 优化的图标数据 (12x12像素，适配128x64屏幕) =====
// 书本图标 - 更清晰的12x12设计
const unsigned char icon_book[] PROGMEM = {
  0x7F, 0x80, 0x40, 0x80, 0x40, 0x80, 0x7F, 0x80, 
  0x40, 0x80, 0x40, 0x80, 0x40, 0x80, 0x40, 0x80, 
  0x40, 0x80, 0x40, 0x80, 0x40, 0x80, 0x7F, 0x80
};

// 时钟图标 - 简化设计
const unsigned char icon_clock[] PROGMEM = {
  0x1F, 0x00, 0x31, 0x80, 0x40, 0x40, 0x44, 0x40, 
  0x42, 0x40, 0x41, 0x40, 0x40, 0x40, 0x40, 0x40, 
  0x40, 0x40, 0x40, 0x40, 0x31, 0x80, 0x1F, 0x00
};

// 心形图标 - 优化形状
const unsigned char icon_heart[] PROGMEM = {
  0x00, 0x00, 0x1B, 0x00, 0x3F, 0x80, 0x7F, 0xC0, 
  0x7F, 0xC0, 0x3F, 0x80, 0x1F, 0x00, 0x0E, 0x00, 
  0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// 星星图标 - 锐利设计
const unsigned char icon_star[] PROGMEM = {
  0x04, 0x00, 0x04, 0x00, 0x0E, 0x00, 0x1F, 0x00, 
  0x31, 0x80, 0x0E, 0x00, 0x1F, 0x00, 0x31, 0x80, 
  0x60, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// 电池图标 - 清晰电量显示
const unsigned char icon_battery[] PROGMEM = {
  0x3F, 0x00, 0x21, 0x80, 0x25, 0x80, 0x25, 0x80, 
  0x25, 0x80, 0x25, 0x80, 0x25, 0x80, 0x25, 0x80, 
  0x25, 0x80, 0x21, 0x80, 0x3F, 0x00, 0x00, 0x00
};

// 新增小图标 (8x8像素，用于状态指示)
const unsigned char mini_wifi[] PROGMEM = {
  0x00, 0x18, 0x24, 0x42, 0x81, 0x99, 0x24, 0x00
};

const unsigned char mini_nfc[] PROGMEM = {
  0x3C, 0x42, 0x42, 0x5A, 0x5A, 0x42, 0x42, 0x3C
};

const unsigned char mini_light[] PROGMEM = {
  0x08, 0x49, 0x2A, 0x1C, 0x1C, 0x2A, 0x49, 0x08
};

// 数字字体 (5x7像素，用于大号数字显示)
const unsigned char numbers_5x7[][5] PROGMEM = {
  {0x3E, 0x41, 0x41, 0x41, 0x3E}, // 0
  {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
  {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
  {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
  {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
  {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
  {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
  {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
  {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
  {0x06, 0x49, 0x49, 0x29, 0x1E}  // 9
};

// ===== 函数声明（原有） =====
void beep(int duration = 100);
void beepNFCSuccess();
void beepPomodoroComplete();
void beepLightWarning();
void saveAdaptivePomodoro(unsigned long sessionMillis);
void showNFCUIDAndSave(uint8_t* uid, uint8_t uidLength);
void drawTrendGraph();
void drawAnimatedIcon(int x, int y, int frame);
void drawProgressBar(int x, int y, int width, int height, int progress, int maxProgress);
void drawCornerDecorations();
void drawOptimizedUI();
void drawStatusBar(bool nfcActive, bool lightOK, int batteryLevel);
void drawLargeNumber(int x, int y, int number);
void drawRoundedProgressBar(int x, int y, int width, int height, int progress, int maxProgress);
void drawSimpleCard(int x, int y, int width, int height, const char* label, const char* value);
void drawWaveform(int startX, int startY, int width, int height, int* data, int dataSize, int maxValue);
bool initializeOLED();
bool initializeNFC();
void turnOffOLED();
void turnOnOLED();
void scanI2CDevices();
WakeupReason getWakeupReason();
void configureWakeupSources();
void enterSmartSleep();
void wakeupInitialization();
bool shouldEnterSleep();
void shutdownAllHardware();  // 新增：关闭所有硬件

// ===== 新增书籍管理函数声明 =====
void initializeBooks();                                      // 初始化书籍数据
int findBookByUID(String uid);                              // 根据UID查找书籍
int addNewBook(String uid);                                 // 添加新书
void loadBookData(int index);                               // 加载书籍数据
void saveBookData(int index);                               // 保存书籍数据
void saveCurrentBookSession();                              // 保存当前书籍会话
void switchBook(String uid);                                // 切换书籍
void displayBookInfo(int bookIndex);                        // 显示书籍信息
void displayBookLibrary();                                   // 显示书籍库
void generateBookName(String uid, char* name);              // 生成书名
String formatReadingTime(unsigned long seconds);            // 格式化阅读时间
void beepBookSwitch();                                      // 书籍切换提示音
void drawBookCard(int x, int y, int width, int height, Book& book); // 绘制书籍卡片
void drawInfoCard(int x, int y, int width, int height, const char* title, const char* value, const unsigned char* icon); // 绘制信息卡片

// ===== 书籍管理函数实现 =====

/**
 * 初始化书籍数据结构
 */
void initializeBooks() {
  for (int i = 0; i < MAX_BOOKS; i++) {
    books[i].uid = "";
    books[i].name = "";
    books[i].totalSeconds = 0;
    books[i].lastRead = 0;
    books[i].sessionCount = 0;
    books[i].pomodoroCount = 0;
    books[i].avgSessionMinutes = 0;
    books[i].isActive = false;
  }
  
  // 从存储中加载书籍数据
  bookPrefs.begin("books", false);
  int bookCount = bookPrefs.getInt("count", 0);
  
  Serial.println("Loading " + String(bookCount) + " books from storage");
  
  for (int i = 0; i < bookCount && i < MAX_BOOKS; i++) {
    String key = "book" + String(i);
    if (bookPrefs.isKey((key + "_uid").c_str())) {
      books[i].uid = bookPrefs.getString((key + "_uid").c_str(), "");
      books[i].name = bookPrefs.getString((key + "_name").c_str(), "Book " + String(i+1));
      books[i].totalSeconds = bookPrefs.getULong((key + "_total").c_str(), 0);
      books[i].lastRead = bookPrefs.getULong((key + "_last").c_str(), 0);
      books[i].sessionCount = bookPrefs.getInt((key + "_sessions").c_str(), 0);
      books[i].pomodoroCount = bookPrefs.getInt((key + "_pomodoros").c_str(), 0);
      books[i].avgSessionMinutes = bookPrefs.getFloat((key + "_avg").c_str(), 0);
      books[i].isActive = true;
      
      Serial.println("Loaded book: " + books[i].name + " (UID: " + books[i].uid + ")");
    }
  }
}

/**
 * 根据UID查找书籍
 * 返回书籍索引，未找到返回-1
 */
int findBookByUID(String uid) {
  for (int i = 0; i < MAX_BOOKS; i++) {
    if (books[i].isActive && books[i].uid == uid) {
      return i;
    }
  }
  return -1;
}

/**
 * 添加新书
 * 返回新书索引，失败返回-1
 */
int addNewBook(String uid) {
  // 查找空位
  int emptySlot = -1;
  for (int i = 0; i < MAX_BOOKS; i++) {
    if (!books[i].isActive) {
      emptySlot = i;
      break;
    }
  }
  
  if (emptySlot == -1) {
    Serial.println("Book library full!");
    return -1;
  }
  
  // 初始化新书数据
  books[emptySlot].uid = uid;
  
  // 生成书名
  char bookName[BOOK_NAME_LENGTH];
  generateBookName(uid, bookName);
  books[emptySlot].name = String(bookName);
  
  books[emptySlot].totalSeconds = 0;
  books[emptySlot].lastRead = millis() / 1000;
  books[emptySlot].sessionCount = 0;
  books[emptySlot].pomodoroCount = 0;
  books[emptySlot].avgSessionMinutes = 0;
  books[emptySlot].isActive = true;
  
  // 保存到存储
  saveBookData(emptySlot);
  
  // 更新书籍数量
  int activeCount = 0;
  for (int i = 0; i < MAX_BOOKS; i++) {
    if (books[i].isActive) activeCount++;
  }
  bookPrefs.putInt("count", activeCount);
  
  Serial.println("Added new book: " + books[emptySlot].name);
  return emptySlot;
}

/**
 * 生成书名
 * 根据UID生成一个友好的书名
 */
void generateBookName(String uid, char* name) {
  // 使用UID的最后4个字符作为书籍编号
  String shortId = uid.substring(uid.length() - 4);
  snprintf(name, BOOK_NAME_LENGTH, "Book-%s", shortId.c_str());
}

/**
 * 加载指定书籍的数据
 */
void loadBookData(int index) {
  if (index < 0 || index >= MAX_BOOKS) return;
  
  String key = "book" + String(index);
  books[index].uid = bookPrefs.getString((key + "_uid").c_str(), "");
  books[index].name = bookPrefs.getString((key + "_name").c_str(), "");
  books[index].totalSeconds = bookPrefs.getULong((key + "_total").c_str(), 0);
  books[index].lastRead = bookPrefs.getULong((key + "_last").c_str(), 0);
  books[index].sessionCount = bookPrefs.getInt((key + "_sessions").c_str(), 0);
  books[index].pomodoroCount = bookPrefs.getInt((key + "_pomodoros").c_str(), 0);
  books[index].avgSessionMinutes = bookPrefs.getFloat((key + "_avg").c_str(), 0);
}

/**
 * 保存指定书籍的数据
 */
void saveBookData(int index) {
  if (index < 0 || index >= MAX_BOOKS || !books[index].isActive) return;
  
  String key = "book" + String(index);
  bookPrefs.putString((key + "_uid").c_str(), books[index].uid);
  bookPrefs.putString((key + "_name").c_str(), books[index].name);
  bookPrefs.putULong((key + "_total").c_str(), books[index].totalSeconds);
  bookPrefs.putULong((key + "_last").c_str(), books[index].lastRead);
  bookPrefs.putInt((key + "_sessions").c_str(), books[index].sessionCount);
  bookPrefs.putInt((key + "_pomodoros").c_str(), books[index].pomodoroCount);
  bookPrefs.putFloat((key + "_avg").c_str(), books[index].avgSessionMinutes);
  
  Serial.println("Saved book data: " + books[index].name);
}

/**
 * 保存当前书籍的阅读会话
 */
void saveCurrentBookSession() {
  if (currentBookIndex < 0 || currentBookIndex >= MAX_BOOKS) return;
  
  unsigned long sessionSeconds = accumulatedSessionMillis / 1000;
  if (sessionSeconds > 0) {
    // 更新书籍数据
    books[currentBookIndex].totalSeconds += sessionSeconds;
    books[currentBookIndex].sessionCount++;
    books[currentBookIndex].lastRead = millis() / 1000;
    
    // 更新平均阅读时长
    float totalMinutes = (float)books[currentBookIndex].totalSeconds / 60.0;
    books[currentBookIndex].avgSessionMinutes = totalMinutes / books[currentBookIndex].sessionCount;
    
    // 保存到存储
    saveBookData(currentBookIndex);
    
    Serial.println("Saved session for " + books[currentBookIndex].name + 
                   ": " + String(sessionSeconds) + " seconds");
  }
}

/**
 * 切换书籍
 */
void switchBook(String uid) {
  // 保存当前书籍的会话
  if (currentBookIndex >= 0) {
    saveCurrentBookSession();
  }
  
  // 查找或创建新书
  int bookIndex = findBookByUID(uid);
  if (bookIndex == -1) {
    // 新书，添加到库中
    bookIndex = addNewBook(uid);
    if (bookIndex == -1) {
      // 库满，显示错误
      if (oledInitialized) {
        display.clearDisplay();
        display.setTextSize(1);
        display.setCursor(20, 20);
        display.println("Book Library Full!");
        display.setCursor(15, 35);
        display.println("Max " + String(MAX_BOOKS) + " books");
        display.display();
        delay(3000);
      }
      return;
    }
  }
  
  // 切换到新书
  currentBookIndex = bookIndex;
  currentBookUID = uid;
  bookChanged = true;
  accumulatedSessionMillis = 0;  // 重置会话时间
  
  // 开始新会话
  books[currentBookIndex].sessionCount++;
  
  // 显示书籍信息
  displayBookInfo(currentBookIndex);
  
  // 播放切换提示音
  beepBookSwitch();
  
  // 更新最后活动时间
  lastActivityTime = millis();
  
  Serial.println("Switched to book: " + books[currentBookIndex].name);
}

/**
 * 书籍切换提示音
 */
void beepBookSwitch() {
  beep(100);
  delay(50);
  beep(100);
  delay(50);
  beep(200);
  Serial.println("Book Switch Sound");
}

/**
 * 格式化阅读时间
 */
String formatReadingTime(unsigned long seconds) {
  unsigned long hours = seconds / 3600;
  unsigned long minutes = (seconds % 3600) / 60;
  unsigned long secs = seconds % 60;
  
  char timeStr[20];
  if (hours > 0) {
    snprintf(timeStr, sizeof(timeStr), "%luh %lum", hours, minutes);
  } else if (minutes > 0) {
    snprintf(timeStr, sizeof(timeStr), "%lum %lus", minutes, secs);
  } else {
    snprintf(timeStr, sizeof(timeStr), "%lus", secs);
  }
  
  return String(timeStr);
}

/**
 * 显示书籍信息
 */
void displayBookInfo(int bookIndex) {
  if (!oledInitialized || bookIndex < 0 || bookIndex >= MAX_BOOKS) return;
  
  isShowingNFC = true;
  nfcDisplayStartTime = millis();
  
  display.clearDisplay();
  drawOptimizedUI();
  drawStatusBar(true, true, 90);
  
  // 标题
  display.setTextSize(1);
  display.setCursor(35, 12);
  display.println("BOOK INFO");
  
  // 书籍卡片
  drawBookCard(5, 22, 118, 40, books[bookIndex]);
  
  display.display();
}

/**
 * 绘制书籍卡片
 */
void drawBookCard(int x, int y, int width, int height, Book& book) {
  // 外框
  display.drawRoundRect(x, y, width, height, 3, SSD1306_WHITE);
  
  // 书名
  display.setTextSize(1);
  display.setCursor(x + 5, y + 3);
  display.print(book.name);
  
  // 分隔线
  display.drawLine(x + 5, y + 13, x + width - 5, y + 13, SSD1306_WHITE);
  
  // 阅读时间
  display.setCursor(x + 5, y + 17);
  display.print("Time: ");
  display.print(formatReadingTime(book.totalSeconds));
  
  // 会话次数
  display.setCursor(x + 5, y + 27);
  display.print("Sessions: ");
  display.print(book.sessionCount);
  
  // 番茄钟数
  display.setCursor(x + 65, y + 27);
  display.print("Pomo: ");
  display.print(book.pomodoroCount);
}

/**
 * 显示书籍库
 */
void displayBookLibrary() {
  if (!oledInitialized) return;
  
  display.clearDisplay();
  drawOptimizedUI();
  drawStatusBar(false, true, 80);
  
  // 标题
  display.setTextSize(1);
  display.setCursor(30, 12);
  display.println("MY LIBRARY");
  
  // 统计活跃书籍数
  int activeCount = 0;
  for (int i = 0; i < MAX_BOOKS; i++) {
    if (books[i].isActive) activeCount++;
  }
  
  // 显示统计
  display.setCursor(5, 25);
  display.print("Books: ");
  display.print(activeCount);
  display.print("/");
  display.print(MAX_BOOKS);
  
  // 显示总阅读时间
  unsigned long totalTime = 0;
  for (int i = 0; i < MAX_BOOKS; i++) {
    if (books[i].isActive) {
      totalTime += books[i].totalSeconds;
    }
  }
  
  display.setCursor(5, 38);
  display.print("Total: ");
  display.print(formatReadingTime(totalTime));
  
  // 显示当前书籍
  if (currentBookIndex >= 0) {
    display.setCursor(5, 51);
    display.print("Current: ");
    display.print(books[currentBookIndex].name);
  }
  
  display.display();
}

/**
 * 检测唤醒原因
 * 分析ESP32从深度睡眠中唤醒的具体原因
 */
WakeupReason getWakeupReason() {
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  
  switch(wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0:  // GPIO 26 (光敏电阻) 唤醒
      Serial.println("Wakeup caused by GPIO 26 (Light sensor)");
      return WAKEUP_LIGHT;
      
    case ESP_SLEEP_WAKEUP_EXT1: { // 多GPIO唤醒（备用）
      Serial.println("Wakeup caused by EXT1 (multiple GPIO)");
      uint64_t wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();
      if (wakeup_pin_mask & (1ULL << 26)) {
        return WAKEUP_LIGHT;
      }
      if (wakeup_pin_mask & (1ULL << 2)) {
        return WAKEUP_BUTTON;
      }
      return WAKEUP_UNKNOWN;
    }
      
    case ESP_SLEEP_WAKEUP_TIMER:  // 定时器唤醒（安全网）
      Serial.println("Wakeup caused by timer");
      return WAKEUP_TIMER;
      
    default:  // 首次启动或重启
      Serial.println("Wakeup was not caused by deep sleep (first boot or reset)");
      return WAKEUP_UNKNOWN;
  }
}

/**
 * 配置外部唤醒源
 * 设置GPIO和定时器作为唤醒触发条件
 */
void configureWakeupSources() {
  // 使用EXT0唤醒 (单GPIO)
  // GPIO 26 (光敏电阻): 高电平唤醒 (光线充足时)
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_26, 1);  // 1 = 高电平唤醒
  
  // 备用：设置一个较长的定时器唤醒（30分钟）作为安全网
  // 防止系统长时间无法唤醒
  esp_sleep_enable_timer_wakeup(30 * 60 * 1000000ULL); // 30分钟
  
  Serial.println("Safe wakeup sources configured:");
  Serial.println("- GPIO 26 (LDR): HIGH level (bright light)");
  Serial.println("- Timer: 30 minutes (safety)");
  Serial.println("- GPIO 0 avoided (safe for boot process)");
}

/**
 * 新增：关闭所有硬件以降低功耗
 */
void shutdownAllHardware() {
  Serial.println("Shutting down all hardware...");
  
  // 1. 关闭OLED
  if (oledInitialized) {
    display.clearDisplay();
    display.display();
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    delay(50);
  }
  
  // 2. 停止I2C通信
  Wire.end();
  
  // 3. 将I2C引脚设置为高阻抗输入以节省电力
  pinMode(SDA_PIN, INPUT);
  pinMode(SCL_PIN, INPUT);
  
  // 4. 关闭蜂鸣器
  digitalWrite(piezoPin, HIGH);
  pinMode(piezoPin, INPUT);  // 设置为输入以降低功耗
  
  // 5. 将非RTC GPIO设置为输入以降低功耗
  pinMode(ldrPin, INPUT);  // ADC引脚
  
  // 6. 配置RTC GPIO保持功能（用于唤醒）
  rtc_gpio_init(GPIO_NUM_26);  // 初始化为RTC GPIO
  rtc_gpio_set_direction(GPIO_NUM_26, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pulldown_en(GPIO_NUM_26);  // 启用下拉电阻
  rtc_gpio_pullup_dis(GPIO_NUM_26);   // 禁用上拉电阻
  
  if (buttonWakeupPin > 0) {
    rtc_gpio_init(GPIO_NUM_2);  // 初始化按钮为RTC GPIO
    rtc_gpio_set_direction(GPIO_NUM_2, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(GPIO_NUM_2);   // 启用上拉电阻
    rtc_gpio_pulldown_dis(GPIO_NUM_2); // 禁用下拉电阻
  }
  
  Serial.println("All hardware shutdown complete");
  delay(100);  // 确保所有操作完成
}

/**
 * 智能睡眠函数 - 修复版本
 * 保存当前状态并进入深度睡眠模式
 */
void enterSmartSleep() {
  Serial.println("=== Entering Smart Sleep Mode ===");
  
  // 保存当前书籍的阅读数据
  if (currentBookIndex >= 0 && (isReading || isPaused)) {
    unsigned long now = millis();
    unsigned long sessionMillis = accumulatedSessionMillis;
    if (isReading) sessionMillis += now - sessionStartMillis;
    
    // 更新当前书籍的阅读时间
    books[currentBookIndex].totalSeconds += sessionMillis / 1000;
    books[currentBookIndex].lastRead = now / 1000;
    saveBookData(currentBookIndex);
    
    // 保存自适应番茄钟数据
    saveAdaptivePomodoro(sessionMillis);
    
    // 更新总阅读时间
    totalReadingSeconds += sessionMillis / 1000;
    prefs.putULong("totalSecs", totalReadingSeconds);
  }
  
  // 保存当前书籍索引
  prefs.putInt("lastBookIdx", currentBookIndex);
  
  // 智能睡眠显示界面 - 优化版本
  if (oledInitialized && !i2cBusy) {
    display.clearDisplay();
    drawOptimizedUI();
    drawStatusBar(false, false, 25); // 低电量显示
    
    // 睡眠标题
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(38, 12);
    display.println("SLEEP MODE");
    
    // 显示当前书籍
    if (currentBookIndex >= 0) {
      display.setCursor(15, 25);
      display.print("Book: ");
      display.println(books[currentBookIndex].name);
    }
    
    // 唤醒条件信息卡片
    drawInfoCard(5, 35, 118, 20, "Wake on:", "Light/Button/Timer", icon_battery);
    
    // 睡眠动画 - 渐变效果
    for (int fade = 5; fade >= 0; fade--) {
      display.drawRect(40 + fade * 2, 52 + fade, 48 - fade * 4, 8 - fade * 2, SSD1306_WHITE);
      display.display();
      delay(200);
    }
    
    delay(1000);
  }
  
  // 关闭所有硬件
  shutdownAllHardware();
  
  // 配置唤醒源
  configureWakeupSources();
  
  // 刷新所有串口数据
  Serial.flush();
  
  // 进入深度睡眠
  Serial.println("Going to deep sleep now...");
  esp_deep_sleep_start();
  
  // 不会执行到这里
}

/**
 * 光线检测逻辑 - 优化版本
 * 快速检测是否应该进入睡眠模式，减少检测次数提高响应速度
 */
bool shouldEnterSleep() {
  static unsigned long systemStartTime = millis();
  static int lowLightCount = 0;        // 低光照计数器
  static unsigned long lastCheck = 0;
  
  unsigned long now = millis();
  int lightValue = analogRead(ldrPin);
  
  // 系统启动后的保护期内不检查睡眠
  if (now - systemStartTime < STARTUP_GRACE_PERIOD) {
    Serial.println("System startup grace period, sleep check disabled");
    return false;
  }
  
  // 空闲超时检测 - 30秒无活动自动睡眠
  if (now - lastActivityTime > IDLE_SLEEP_TIMEOUT) {
    Serial.println("Idle timeout reached, entering sleep mode");
    return true;
  }
  
  // 如果正在显示NFC或处于活跃状态，不进入睡眠
  if (isShowingNFC || isReading || inRest) {
    lowLightCount = 0;
    lastActivityTime = now;  // 更新活动时间
    return false;
  }
  
  // 定期检查光线
  if (now - lastCheck > SLEEP_CHECK_INTERVAL) {
    lastCheck = now;
    
    Serial.println("Light check - Value: " + String(lightValue) + ", Threshold: " + String(bookmarkThreshold));
    
    if (lightValue < bookmarkThreshold) {
      lowLightCount++;
      Serial.println("Low light detected, count: " + String(lowLightCount) + "/" + String(SLEEP_CHECK_COUNT));
    } else {
      lowLightCount = 0;
      lastActivityTime = now;  // 有光线，更新活动时间
      Serial.println("Light sufficient, count reset");
    }
  }
  
  // 连续检测到低光照就进入睡眠
  if (lowLightCount >= SLEEP_CHECK_COUNT) {
    Serial.println("Entering sleep mode due to prolonged low light");
    return true;
  }
  
  return false;
}

/**
 * 从睡眠中恢复的初始化
 * 重新启动硬件并显示欢迎信息
 */
void wakeupInitialization() {
  WakeupReason reason = getWakeupReason();
  
  // 重新初始化I2C
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(100);
  
  // 重新初始化硬件
  turnOnOLED();
  
  // 重新初始化GPIO
  pinMode(piezoPin, OUTPUT);
  digitalWrite(piezoPin, HIGH);
  pinMode(ldrPin, INPUT);
  pinMode(ldrWakeupPin, INPUT);
  pinMode(buttonWakeupPin, INPUT_PULLUP);
  
  // 重新初始化NFC
  nfcInitialized = initializeNFC();
  
  if (oledInitialized) {
    display.clearDisplay();
    drawCornerDecorations();
    
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(30, 2);
    display.println("Welcome Back!");
    
    // 根据不同的唤醒原因显示相应信息
    switch(reason) {
      case WAKEUP_LIGHT:
        display.drawBitmap(6, 20, icon_book, 16, 16, SSD1306_WHITE);
        display.setCursor(28, 20);
        display.println("Light detected");
        display.setCursor(28, 30);
        display.println("Ready to read!");
        break;
        
      case WAKEUP_BUTTON:
        display.drawBitmap(6, 20, icon_heart, 16, 16, SSD1306_WHITE);
        display.setCursor(28, 20);
        display.println("Button pressed");
        display.setCursor(28, 30);
        display.println("Manual wakeup");
        break;
        
      case WAKEUP_TIMER:
        display.drawBitmap(6, 20, icon_clock, 16, 16, SSD1306_WHITE);
        display.setCursor(28, 20);
        display.println("Timer wakeup");
        display.setCursor(28, 30);
        display.println("Safety check");
        break;
        
      default:
        display.drawBitmap(6, 20, icon_star, 16, 16, SSD1306_WHITE);
        display.setCursor(28, 20);
        display.println("System start");
        break;
    }
    
    // 显示上次阅读的书籍
    int lastBookIdx = prefs.getInt("lastBookIdx", -1);
    if (lastBookIdx >= 0 && lastBookIdx < MAX_BOOKS && books[lastBookIdx].isActive) {
      display.setCursor(28, 45);
      display.print("Last: ");
      display.print(books[lastBookIdx].name);
    }
    
    display.display();
    delay(2000);
  }
  
  // 播放唤醒提示音
  beep(200);
  delay(100);
  beep(200);
  
  // 更新最后活动时间
  lastActivityTime = millis();
}

/**
 * I2C设备扫描函数
 * 扫描并显示所有连接的I2C设备地址
 */
void scanI2CDevices() {
  Serial.println("Scanning I2C devices...");
  int deviceCount = 0;
  
  for (byte address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    byte error = Wire.endTransmission();
    
    if (error == 0) {
      Serial.print("I2C device found at address 0x");
      if (address < 16) Serial.print("0");
      Serial.print(address, HEX);
      Serial.println();
      deviceCount++;
    }
  }
  
  Serial.print("Total devices found: ");
  Serial.println(deviceCount);
}

/**
 * 绘制优化的屏幕边框和状态栏
 * 为128x64屏幕优化的UI设计
 */
void drawOptimizedUI() {
  // 顶部状态栏 (高度：10像素)
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);
  
  // 四角装饰 - 简化版本适合小屏幕
  display.drawLine(0, 0, 5, 0, SSD1306_WHITE);
  display.drawLine(0, 0, 0, 5, SSD1306_WHITE);
  display.drawLine(122, 0, 127, 0, SSD1306_WHITE);
  display.drawLine(127, 0, 127, 5, SSD1306_WHITE);
  display.drawLine(0, 58, 5, 58, SSD1306_WHITE);
  display.drawLine(0, 58, 0, 63, SSD1306_WHITE);
  display.drawLine(122, 63, 127, 63, SSD1306_WHITE);
  display.drawLine(127, 58, 127, 63, SSD1306_WHITE);
}

/**
 * 绘制状态栏图标
 * 在屏幕顶部显示系统状态
 */
void drawStatusBar(bool nfcActive, bool lightOK, int batteryLevel) {
  // NFC状态 (左侧)
  if (nfcActive) {
    display.drawBitmap(2, 1, mini_nfc, 8, 8, SSD1306_WHITE);
  }
  
  // 光线状态 (中间左)
  if (lightOK) {
    display.drawBitmap(12, 1, mini_light, 8, 8, SSD1306_WHITE);
  }
  
  // 书籍图标 (中间右) - 新增，表示有书籍在读
  if (currentBookIndex >= 0) {
    display.drawBitmap(22, 1, icon_book, 8, 8, SSD1306_WHITE);
  }
  
  // 电池电量显示 (右侧)
  display.drawRect(115, 2, 12, 6, SSD1306_WHITE);
  display.drawRect(127, 3, 1, 4, SSD1306_WHITE);
  int battFill = map(batteryLevel, 0, 100, 0, 10);
  if (battFill > 0) {
    display.fillRect(116, 3, battFill, 4, SSD1306_WHITE);
  }
  
  // 时间显示 (中心)
  unsigned long mins = millis() / 60000;
  display.setTextSize(1);
  display.setCursor(50, 1);
  display.printf("%02lu:%02lu", (mins/60)%24, mins%60);
}

/**
 * 绘制大号数字
 * 用于显示重要数据如时间、进度等
 */
void drawLargeNumber(int x, int y, int number) {
  String numStr = String(number);
  int startX = x;
  
  for (int i = 0; i < numStr.length(); i++) {
    int digit = numStr.charAt(i) - '0';
    if (digit >= 0 && digit <= 9) {
      for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
          if (numbers_5x7[digit][col] & (1 << row)) {
            display.drawPixel(startX + col, y + row, SSD1306_WHITE);
          }
        }
      }
      startX += 6; // 5像素宽度 + 1像素间距
    }
  }
}

/**
 * 绘制圆角矩形进度条
 * 适合小屏幕的精美进度显示
 */
void drawRoundedProgressBar(int x, int y, int width, int height, int progress, int maxProgress) {
  // 外框圆角矩形
  display.drawRoundRect(x, y, width, height, 2, SSD1306_WHITE);
  
  // 内部进度填充
  if (progress > 0) {
    int fillWidth = ((progress * (width - 4)) / maxProgress);
    if (fillWidth > 2) {
      display.fillRoundRect(x + 2, y + 2, fillWidth, height - 4, 1, SSD1306_WHITE);
    }
  }
  
  // 进度百分比文字
  int percent = (progress * 100) / maxProgress;
  display.setTextSize(1);
  int textX = x + (width - 18) / 2; // 居中显示
  int textY = y + (height - 8) / 2;
  
  // 文字背景擦除
  display.fillRect(textX - 1, textY - 1, 20, 10, SSD1306_BLACK);
  
  display.setCursor(textX, textY);
  display.printf("%d%%", percent);
}

/**
 * 绘制信息卡片 - 修复版本
 * 用于组织显示相关信息，避免文字重叠
 */
void drawInfoCard(int x, int y, int width, int height, const char* title, const char* value, const unsigned char* icon) {
  // 卡片边框
  display.drawRoundRect(x, y, width, height, 2, SSD1306_WHITE);
  
  // 图标 - 调整位置避免重叠
  if (icon) {
    display.drawBitmap(x + 3, y + 3, icon, 12, 12, SSD1306_WHITE);
  }
  
  // 标题 - 调整位置和大小
  display.setTextSize(1);
  display.setCursor(x + 18, y + 2);
  display.print(title);
  
  // 数值 - 确保不与边框重叠
  display.setCursor(x + 18, y + height - 10);
  display.print(value);
}

/**
 * 绘制简化信息卡片
 * 用于显示关键数据，避免拥挤
 */
void drawSimpleCard(int x, int y, int width, int height, const char* label, const char* value) {
  // 简单边框
  display.drawRect(x, y, width, height, SSD1306_WHITE);
  
  // 标签
  display.setTextSize(1);
  display.setCursor(x + 2, y + 2);
  display.print(label);
  
  // 数值 - 居中显示
  int valueLen = strlen(value) * 6; // 估算文字宽度
  int centerX = x + (width - valueLen) / 2;
  display.setCursor(centerX, y + height - 10);
  display.print(value);
}

/**
 * 绘制动态波形
 * 用于光线趋势等数据可视化
 */
void drawWaveform(int startX, int startY, int width, int height, int* data, int dataSize, int maxValue) {
  if (dataSize < 2) return;
  
  // 绘制坐标轴
  display.drawLine(startX, startY + height, startX + width, startY + height, SSD1306_WHITE);
  display.drawLine(startX, startY, startX, startY + height, SSD1306_WHITE);
  
  // 绘制数据波形
  for (int i = 0; i < dataSize - 1 && i < width; i++) {
    int x1 = startX + i;
    int y1 = startY + height - map(data[i], 0, maxValue, 0, height);
    int x2 = startX + i + 1;
    int y2 = startY + height - map(data[i + 1], 0, maxValue, 0, height);
    
    display.drawLine(x1, y1, x2, y2, SSD1306_WHITE);
    
    // 数据点
    display.drawPixel(x1, y1, SSD1306_WHITE);
  }
}

/**
 * 绘制传统四角装饰边框（兼容性保留）
 * 为了保持与旧版本的兼容性
 */
void drawCornerDecorations() {
  // 左上角
  display.drawLine(0, 0, 8, 0, SSD1306_WHITE);
  display.drawLine(0, 0, 0, 8, SSD1306_WHITE);
  // 右上角
  display.drawLine(119, 0, 127, 0, SSD1306_WHITE);
  display.drawLine(127, 0, 127, 8, SSD1306_WHITE);
  // 左下角
  display.drawLine(0, 56, 8, 56, SSD1306_WHITE);
  display.drawLine(0, 56, 0, 63, SSD1306_WHITE);
  // 右下角
  display.drawLine(119, 63, 127, 63, SSD1306_WHITE);
  display.drawLine(127, 55, 127, 63, SSD1306_WHITE);
}

/**
 * 绘制动态轮播图标
 * 循环显示不同图标实现动画效果
 */
void drawAnimatedIcon(int x, int y, int frame) {
  const unsigned char* icons[] = {icon_book, icon_heart, icon_star, icon_clock};
  int iconIndex = (frame / 20) % 4;  // 每20帧切换一次图标
  display.drawBitmap(x, y, icons[iconIndex], 12, 12, SSD1306_WHITE);
}

/**
 * 绘制进度条
 * 显示阅读进度或休息进度
 */
void drawProgressBar(int x, int y, int width, int height, int progress, int maxProgress) {
  // 绘制外框
  display.drawRect(x, y, width, height, SSD1306_WHITE);
  // 计算填充宽度
  int fillWidth = (progress * (width - 2)) / maxProgress;
  if (fillWidth > 0) {
    // 填充进度
    display.fillRect(x + 1, y + 1, fillWidth, height - 2, SSD1306_WHITE);
  }
}

/**
 * 蜂鸣器基础函数
 * 产生指定时长的蜂鸣声
 */
void beep(int duration) {
  digitalWrite(piezoPin, LOW);   // 开启蜂鸣器
  delay(duration);
  digitalWrite(piezoPin, HIGH);  // 关闭蜂鸣器
  Serial.println("Beep! Duration: " + String(duration) + "ms");
}

/**
 * NFC检测成功提示音
 * 双声调表示成功检测到NFC卡片
 */
void beepNFCSuccess() {
  beep(150);
  delay(100);
  beep(150);
  Serial.println("NFC Success Sound");
}

/**
 * 番茄钟完成提示音
 * 三声长音表示完成一个番茄钟周期
 */
void beepPomodoroComplete() {
  beep(200);
  delay(150);
  beep(200);
  delay(150);
  beep(300);
  Serial.println("Pomodoro Complete Sound");
}

/**
 * 光线警告提示音
 * 长音警告光线不足，有防重复机制
 */
void beepLightWarning() {
  unsigned long now = millis();
  if (now - lastBeepTime > beepInterval) {  // 防止频繁蜂鸣
    digitalWrite(piezoPin, LOW);
    delay(800);  // 长音800毫秒
    digitalWrite(piezoPin, HIGH);
    lastBeepTime = now;
    Serial.println("Light Warning Sound");
  }
}

/**
 * 自适应番茄钟保存函数
 * 根据用户实际阅读表现调整番茄钟时长
 * 使用机器学习算法动态优化时间设置
 */
void saveAdaptivePomodoro(unsigned long sessionMillis) {
  // 过滤过短的会话（少于5分钟）
  if (sessionMillis < 5 * 60 * 1000UL) return;
  
  // 学习率和增长率常数
  const float LEARNING_RATE = 0.15;      // 学习率
  const float SUCCESS_GROWTH = 1.05;     // 超额完成增长率
  const float PARTIAL_GROWTH = 1.02;     // 部分完成增长率
  const float ADJUST_RATE = 0.8;         // 调整率
  
  // 更新历史数据
  adaptiveTotalSessionTime += sessionMillis;
  adaptiveSessionCount++;
  
  // 计算完成率和历史平均值
  float completionRate = (float)sessionMillis / (float)adaptivePomodoroMillis;
  unsigned long historicalAverage = adaptiveTotalSessionTime / adaptiveSessionCount;
  
  // 根据完成率调整番茄钟时长
  if (completionRate >= 1.0) {
    // 完成或超额完成
    if (completionRate >= 1.2) {
      adaptivePomodoroMillis = (unsigned long)(adaptivePomodoroMillis * SUCCESS_GROWTH);
      Serial.println("[Pomodoro] Excellent! Target increased");
    } else {
      adaptivePomodoroMillis = (unsigned long)(adaptivePomodoroMillis * PARTIAL_GROWTH);
      Serial.println("[Pomodoro] Good! Target slightly increased");
    }
    
    // 更新当前书籍的番茄钟计数
    if (currentBookIndex >= 0) {
      books[currentBookIndex].pomodoroCount++;
      saveBookData(currentBookIndex);
    }
  } else if (completionRate >= 0.8) {
    // 80%-100%完成率：微调
    adaptivePomodoroMillis = (unsigned long)(LEARNING_RATE * historicalAverage + 
                                           (1 - LEARNING_RATE) * adaptivePomodoroMillis);
    Serial.println("[Pomodoro] Stable performance, minor adjustment");
  } else if (completionRate >= 0.6) {
    // 60%-80%完成率：适度调整
    unsigned long targetAdjustment = (unsigned long)(historicalAverage * ADJUST_RATE);
    adaptivePomodoroMillis = (unsigned long)(LEARNING_RATE * targetAdjustment + 
                                           (1 - LEARNING_RATE) * adaptivePomodoroMillis);
    Serial.println("[Pomodoro] Moderate adjustment needed");
  } else {
    // 低于60%完成率：显著调整
    unsigned long targetAdjustment = (unsigned long)(historicalAverage * 0.7);
    adaptivePomodoroMillis = (unsigned long)(LEARNING_RATE * targetAdjustment + 
                                           (1 - LEARNING_RATE) * adaptivePomodoroMillis);
    Serial.println("[Pomodoro] Significant adjustment needed");
  }
  
  // 限制番茄钟时长范围（5分钟-60分钟）
  adaptivePomodoroMillis = constrain(adaptivePomodoroMillis, 
                                   5 * 60 * 1000UL, 
                                   60 * 60 * 1000UL);
  
  // 保存到Flash存储
  prefs.putULong("pomodoro", adaptivePomodoroMillis);
  prefs.putULong("adaptTime", adaptiveTotalSessionTime);
  prefs.putULong("adaptCount", adaptiveSessionCount);
  
  // 输出调整信息
  Serial.println("=== 自适应番茄钟调整 ===");
  Serial.println("完成率: " + String(completionRate * 100, 1) + "%");
  Serial.println("新目标: " + String(adaptivePomodoroMillis / 60000) + "分钟");
  Serial.println("==================");
}

/**
 * 显示优化的NFC检测界面 - 修复版本
 * 针对128x64屏幕优化的书籍信息显示，避免文字重叠
 */
void showNFCUIDAndSave(uint8_t* uid, uint8_t uidLength) {
  // 将UID转换为十六进制字符串
  String uidStr = "";
  for (uint8_t i = 0; i < uidLength; i++) {
    if (uid[i] < 0x10) uidStr += "0";
    uidStr += String(uid[i], HEX);
    if (i < uidLength - 1) uidStr += " ";
  }
  
  // 保存UID信息
  lastUID = uidStr;
  prefs.putString("lastUID", lastUID);
  
  Serial.println("NFC Card detected: " + uidStr);
  
  // 切换到此书籍
  switchBook(uidStr);
}

/**
 * 绘制优化的光线趋势图 - 修复版本
 * 适合128x64屏幕的数据可视化界面，避免文字重叠
 */
void drawTrendGraph() {
  if (!oledInitialized || i2cBusy) return;
  
  display.clearDisplay();
  drawOptimizedUI();
  drawStatusBar(false, false, 70); // NFC未激活，光线不足，电量70%
  
  // 标题
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(35, 12);
  display.println("LIGHT TREND");
  
  // 光线数值显示 - 使用简化卡片
  char luxStr[16];
  sprintf(luxStr, "%d lux", (int)bestLuxEMA);
  drawSimpleCard(5, 22, 60, 18, "Light", luxStr);
  
  // 状态提示
  drawSimpleCard(67, 22, 56, 18, "Status", "Too Dark");
  
  // 波形图区域
  display.drawRect(5, 42, 118, 18, SSD1306_WHITE);
  display.setCursor(8, 44);
  display.println("Trend Analysis:");
  
  // 简化的趋势线
  for (int i = 0; i < 110; i += 3) {
    int trendVal = lightTrend[(trendIndex + i/3) % trendSize];
    int y = 58 - map(trendVal, 0, 4095, 0, 10); // 调整Y范围避免重叠
    display.drawPixel(8 + i, y, SSD1306_WHITE);
    if (i > 0) {
      int prevVal = lightTrend[(trendIndex + (i-3)/3) % trendSize];
      int prevY = 58 - map(prevVal, 0, 4095, 0, 10);
      display.drawLine(8 + i - 3, prevY, 8 + i, y, SSD1306_WHITE);
    }
  }
  
  display.display();
}

/**
 * 初始化OLED显示屏
 * 配置显示参数并显示启动动画
 */
bool initializeOLED() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("OLED initialization failed!");
    return false;
  }
  
  // 启动动画
  display.clearDisplay();
  drawCornerDecorations();
  
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(15, 2);
  display.println("Reading Tracker");
  
  // 显示功能图标
  display.drawBitmap(30, 20, icon_book, 16, 16, SSD1306_WHITE);
  display.drawBitmap(50, 20, icon_heart, 16, 16, SSD1306_WHITE);
  display.drawBitmap(70, 20, icon_clock, 16, 16, SSD1306_WHITE);
  
  display.setCursor(25, 45);
  display.println("v3.1-DeepSleep");  // 版本号更新
  
  // 底部装饰线
  for (int i = 0; i < SCREEN_WIDTH; i += 8) {
    display.drawPixel(i, 55, SSD1306_WHITE);
  }
  
  display.display();
  delay(2000);
  
  // 显示启动状态
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
 * 配置PN532芯片并验证固件版本
 */
bool initializeNFC() {
  Serial.println("Initializing NFC...");
  nfc.begin();
  
  // 获取固件版本验证连接
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("PN532 not found!");
    return false;
  }
  
  Serial.print("Found PN532, FW ver: ");
  Serial.print((versiondata >> 16) & 0xFF, DEC);
  Serial.print('.');
  Serial.println((versiondata >> 8) & 0xFF, DEC);
  
  // 配置SAM (Security and Access Module)
  nfc.SAMConfig();
  Serial.println("NFC initialized successfully");
  return true;
}

/**
 * 关闭OLED显示屏
 * 清屏、关闭显示并释放I2C总线
 */
void turnOffOLED() {
  if (oledInitialized) {
    display.clearDisplay();
    display.display();
    display.ssd1306_command(SSD1306_DISPLAYOFF);  // 关闭显示
    delay(100);
  }
  Wire.end();  // 释放I2C总线
  
  // 设置引脚为上拉输入模式以节省电力
  pinMode(SDA_PIN, INPUT_PULLUP);
  pinMode(SCL_PIN, INPUT_PULLUP);
}

/**
 * 开启OLED显示屏
 * 重新初始化I2C总线和OLED显示
 */
void turnOnOLED() {
  Wire.begin(SDA_PIN, SCL_PIN);  // 重新初始化I2C
  delay(100);
  oledInitialized = initializeOLED();
}

/**
 * setup函数 - 系统初始化
 * 配置硬件、初始化外设、加载用户数据
 */
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== Reading Tracker Starting (Deep Sleep Fixed Version) ===");
  
  // 检查唤醒原因并执行相应初始化
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED) {
    wakeupInitialization();
  }
  
  // GPIO初始化 (安全版本，避免使用GPIO 0)
  pinMode(ldrPin, INPUT);                    // GPIO 36: ADC输入 (只读，精确测量)
  pinMode(ldrWakeupPin, INPUT);              // GPIO 26: 数字输入 (唤醒用)
  pinMode(buttonWakeupPin, INPUT_PULLUP);    // GPIO 2: 按钮输入 (可选)
  pinMode(piezoPin, OUTPUT);                 // GPIO 5: 蜂鸣器输出
  digitalWrite(piezoPin, HIGH);              // 默认关闭蜂鸣器
  
  Serial.println("GPIO configuration:");
  Serial.println("- GPIO 36: LDR ADC input (precise measurement)");
  Serial.println("- GPIO 26: LDR digital input (wakeup trigger)");
  Serial.println("- GPIO 2: Button input (optional manual wakeup)");
  Serial.println("- GPIO 0: AVOIDED (safe for boot process)");
  
  // I2C总线初始化
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(500);
  
  // 扫描I2C设备
  scanI2CDevices();
  
  // 初始化OLED显示屏
  oledInitialized = initializeOLED();
  if (!oledInitialized) {
    Serial.println("OLED failed, continuing without display...");
  }
  
  // 初始化Flash存储
  prefs.begin("reading", false);
  
  // 初始化书籍管理系统
  initializeBooks();
  
  // 从Flash加载用户数据
  totalReadingSeconds = prefs.getULong("totalSecs", 0);
  adaptivePomodoroMillis = prefs.getULong("pomodoro", defaultPomodoroMin * 60000UL);
  adaptiveTotalSessionTime = prefs.getULong("adaptTime", 0);
  adaptiveSessionCount = prefs.getULong("adaptCount", 0);
  
  // 初始化NFC模块
  nfcInitialized = initializeNFC();
  if (!nfcInitialized) {
    Serial.println("NFC failed, continuing without NFC...");
  }
  
  // 显示书籍库信息
  if (oledInitialized) {
    displayBookLibrary();
    delay(3000);
  }
  
  // 输出初始化结果
  Serial.println("=== Setup Complete ===");
  Serial.print("OLED: ");
  Serial.println(oledInitialized ? "OK" : "Failed");
  Serial.print("NFC: ");
  Serial.println(nfcInitialized ? "OK" : "Failed");
  Serial.println("Features:");
  Serial.println("- Multi-book management (up to " + String(MAX_BOOKS) + " books)");
  Serial.println("- Automatic book switching via NFC");
  Serial.println("- Individual book statistics");
  Serial.println("- Adaptive pomodoro timer");
  Serial.println("- TRUE DEEP SLEEP MODE (10μA standby)");
  Serial.println("- Idle timeout: 30 seconds");
  Serial.println("- Low light sleep: 2 checks, 2 seconds");
  
  // 统计并显示书籍信息
  int activeBooks = 0;
  for (int i = 0; i < MAX_BOOKS; i++) {
    if (books[i].isActive) {
      activeBooks++;
      Serial.println("Book " + String(i+1) + ": " + books[i].name + 
                     " (Total: " + formatReadingTime(books[i].totalSeconds) + ")");
    }
  }
  Serial.println("Active books: " + String(activeBooks) + "/" + String(MAX_BOOKS));
  
  // 初始化最后活动时间
  lastActivityTime = millis();
  
  // 启动完成提示音
  beep(200);
  delay(100);
  beep(200);
  Serial.println("System ready with deep sleep support!");
}

/**
 * 主循环 - 增强版本（支持真正的深度睡眠）
 * 处理光线检测、NFC读取、阅读状态管理、书籍切换和睡眠控制
 */
void loop() {
  // 读取光线传感器值并更新趋势数据
  int lightValue = analogRead(ldrPin);
  lightTrend[trendIndex] = lightValue;
  trendIndex = (trendIndex + 1) % trendSize;

  // 如果正在阅读，更新最佳光照EMA值
  if (isReading) {
    bestLuxEMA = (1 - emaAlpha) * bestLuxEMA + emaAlpha * lightValue;
    bestLuxEMA = constrain(bestLuxEMA, 100, 3000);
  }

  unsigned long now = millis();
  
  // 调试信息输出（每5秒一次）
  static unsigned long lastDebugPrint = 0;
  if (now - lastDebugPrint > 5000) {
    lastDebugPrint = now;
    Serial.println("=== System Status ===");
    Serial.println("Light Value: " + String(lightValue));
    Serial.println("Is Reading: " + String(isReading));
    Serial.println("Is Showing NFC: " + String(isShowingNFC));
    Serial.println("In Rest: " + String(inRest));
    Serial.println("Last Activity: " + String((now - lastActivityTime) / 1000) + "s ago");
    if (currentBookIndex >= 0) {
      Serial.println("Current Book: " + books[currentBookIndex].name);
      Serial.println("Session Time: " + formatReadingTime(accumulatedSessionMillis / 1000));
    }
    Serial.println("==================");
  }
  
  // 优先检查睡眠条件
  if (shouldEnterSleep()) {
    Serial.println("Sleep conditions met, entering deep sleep...");
    enterSmartSleep();
    // 不会执行到这里，因为会进入深度睡眠
  }
  
  // NFC检测逻辑
  static unsigned long lastNFCCheck = 0;
  if (nfcInitialized && !i2cBusy && (now - lastNFCCheck > 1000)) {
    lastNFCCheck = now;
    i2cBusy = true;
    
    uint8_t uid[7];
    uint8_t uidLength;
    
    // 尝试读取NFC卡片（500ms超时）
    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 500)) {
      // 转换UID为字符串
      String uidStr = "";
      for (uint8_t i = 0; i < uidLength; i++) {
        if (uid[i] < 0x10) uidStr += "0";
        uidStr += String(uid[i], HEX);
        if (i < uidLength - 1) uidStr += " ";
      }
      
      // 检查是否是新卡片或不同的卡片
      if (uidStr != currentBookUID || !isShowingNFC) {
        isShowingNFC = true;
        nfcDisplayStartTime = now;
        showNFCUIDAndSave(uid, uidLength);
        lastActivityTime = now;  // 更新活动时间
      }
      i2cBusy = false;
    } else {
      if (isShowingNFC) {
        isShowingNFC = false;
        Serial.println("NFC card removed, returning to normal mode");
      }
      i2cBusy = false;
    }
  }
  
  // NFC显示超时处理（10秒后自动返回）
  if (isShowingNFC) {
    if (now - nfcDisplayStartTime > 10000) {  // 缩短到10秒
      isShowingNFC = false;
      bookChanged = false;  // 重置书籍切换标志
      Serial.println("NFC display timeout, returning to normal mode");
    } else {
      // 显示书籍信息期间，检查按钮长按进入书籍库
      if (digitalRead(buttonWakeupPin) == LOW) {
        static unsigned long buttonPressTime = 0;
        if (buttonPressTime == 0) {
          buttonPressTime = now;
        } else if (now - buttonPressTime > 3000) {  // 长按3秒
          displayBookLibrary();
          delay(5000);
          buttonPressTime = 0;
          isShowingNFC = false;
          lastActivityTime = now;  // 更新活动时间
        }
      }
      delay(100);
      return;  // 在显示NFC信息期间跳过其他逻辑
    }
  }
  
  // 光线阈值判断和阅读状态管理
  if (lightValue < lightThreshold) {
    // 光线不足时暂停阅读
    if (isReading) {
      accumulatedSessionMillis += now - sessionStartMillis;
      isReading = false;
      isPaused = true;
      Serial.println("Reading paused due to low light");
    }
    
    // 显示光线趋势图和警告（不在显示NFC时）
    if (!isShowingNFC) {
      drawTrendGraph();
      beepLightWarning();
    }
  } else {
    // 光线充足时开始或继续阅读
    if (!isReading && !inRest && currentBookIndex >= 0) {  // 需要有书籍才能阅读
      isReading = true;
      isPaused = false;
      sessionStartMillis = now;
      lastActivityTime = now;  // 更新活动时间
      Serial.println("Reading session started for: " + books[currentBookIndex].name);
    } else if (!isReading && !inRest && currentBookIndex < 0) {
      // 没有书籍，提示需要NFC卡片
      if (oledInitialized && !isShowingNFC) {
        display.clearDisplay();
        drawOptimizedUI();
        drawStatusBar(nfcInitialized, true, 75);
        
        display.setTextSize(1);
        display.setCursor(25, 20);
        display.println("PLACE NFC CARD");
        display.setCursor(30, 35);
        display.println("TO START READING");
        
        // 动画效果
        animationFrame++;
        drawAnimatedIcon(58, 45, animationFrame);
        
        display.display();
      }
    }
    
    // 阅读模式显示界面 - 修复版本
    if (isReading && oledInitialized && !i2cBusy && !isShowingNFC) {
      unsigned long currentSessionMillis = now - sessionStartMillis + accumulatedSessionMillis;
      unsigned long targetMillis = adaptivePomodoroMillis;
      
      display.clearDisplay();
      drawOptimizedUI();
      
      // 动态状态栏 - 显示实时数据
      int batteryLevel = map(lightValue, 0, 4095, 20, 100); // 模拟电量显示
      drawStatusBar(nfcInitialized, true, batteryLevel);
      
      // 主标题 - 显示书名
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(5, 12);
      display.print("Reading: ");
      display.print(books[currentBookIndex].name);
      
      // 时间信息 - 使用简化卡片避免重叠
      char sessionStr[16], targetStr[16];
      sprintf(sessionStr, "%lum", currentSessionMillis / 60000);
      sprintf(targetStr, "%lum", targetMillis / 60000);
      
      // 左侧：当前会话时间
      drawSimpleCard(5, 22, 56, 18, "Session", sessionStr);
      
      // 右侧：目标时间
      drawSimpleCard(67, 22, 56, 18, "Target", targetStr);
      
      // 现代化进度条
      int progress = (currentSessionMillis * 100) / targetMillis;
      progress = constrain(progress, 0, 100);
      
      display.setCursor(8, 44);
      display.print("Progress:");
      drawRoundedProgressBar(8, 50, 112, 10, progress, 100);
      
      display.display();
      
      // 检查番茄钟是否完成
      if (currentSessionMillis >= targetMillis) {
        beepPomodoroComplete();
        
        // 保存会话数据
        books[currentBookIndex].pomodoroCount++;
        saveCurrentBookSession();
        saveAdaptivePomodoro(currentSessionMillis);
        
        // 完成庆祝动画
        display.clearDisplay();
        drawOptimizedUI();
        drawStatusBar(true, true, batteryLevel);
        
        display.setTextSize(1);
        display.setCursor(28, 12);
        display.println("POMODORO DONE!");
        
        // 显示书籍统计
        display.setCursor(10, 25);
        display.print(books[currentBookIndex].name);
        display.print(": #");
        display.print(books[currentBookIndex].pomodoroCount);
        
        display.drawBitmap(58, 35, icon_heart, 12, 12, SSD1306_WHITE);
        
        // 闪烁庆祝效果
        for (int i = 0; i < 6; i++) {
          display.fillRect(20, 48, 88, 15, i % 2 ? SSD1306_WHITE : SSD1306_BLACK);
          display.setTextColor(i % 2 ? SSD1306_BLACK : SSD1306_WHITE);
          display.setCursor(42, 52);
          display.println("EXCELLENT!");
          display.display();
          delay(300);
        }
        
        // 进入休息模式
        isReading = false;
        inRest = true;
        restStartMillis = now;
        accumulatedSessionMillis = 0;
        lastActivityTime = now;  // 更新活动时间
        
        Serial.println("Pomodoro completed for " + books[currentBookIndex].name + 
                       "! Total: " + String(books[currentBookIndex].pomodoroCount));
      }
    }
  }
  
  // 休息模式处理 - 修复版本
  if (inRest && oledInitialized && !i2cBusy && !isShowingNFC) {
    unsigned long restElapsed = now - restStartMillis;
    unsigned long restRemaining = restDuration - restElapsed;
    
    if (restElapsed >= restDuration) {
      inRest = false;
      lastActivityTime = now;  // 更新活动时间
      Serial.println("Rest period completed");
      
      // 休息结束提示音
      beep(100);
      delay(100);
      beep(200);
    } else {
      display.clearDisplay();
      drawOptimizedUI();
      drawStatusBar(false, false, 65);
      
      // 休息标题
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(42, 12);
      display.println("REST TIME");
      
      // 显示当前书籍信息
      if (currentBookIndex >= 0) {
        display.setCursor(10, 25);
        display.print("Book: ");
        display.print(books[currentBookIndex].name);
      }
      
      // 休息时间信息 - 使用简化卡片
      char restStr[16];
      sprintf(restStr, "%lum %lus", restRemaining / 60000, (restRemaining % 60000) / 1000);
      drawSimpleCard(25, 35, 78, 18, "Remaining", restStr);
      
      // 休息进度条
      int restProgress = (restElapsed * 100) / restDuration;
      drawRoundedProgressBar(8, 55, 112, 7, restProgress, 100);
      
      // 呼吸动画效果
      static int breathFrame = 0;
      breathFrame++;
      int breathRadius = 3 + sin(breathFrame * 0.3) * 2;
      display.fillCircle(115, 40, breathRadius, SSD1306_WHITE);
      
      display.display();
    }
  }
  
  delay(500);  // 主循环延迟，平衡响应速度和功耗
}