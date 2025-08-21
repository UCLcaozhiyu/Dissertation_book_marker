#  From Smart Bookmark to Intelligent Bookstand Low-Power Embedded System Design for Enhanced Paper-Based Reading

An intelligent reading-time tracking device based on ESP32 that automatically recognizes different books through NFC, records reading time, and provides Pomodoro timer, statistical analysis, and other features.

Originally conceived as an ultra-thin solar-powered smart bookmark, the project evolved into a desktop-style smart bookstand to overcome energy and hardware constraints while enhancing usability and user experience.

##  Key Features

###  Multi-Book Management
- Support for tracking up to 10 books independently
- Automatic book recognition and switching via NFC cards
- Individual reading statistics for each book
- Persistent data storage, no data loss on power off

###  Smart Tracking
- Automatic reading state detection via light sensor
- Real-time reading time recording
- Adaptive Pomodoro timer functionality
- Personalized reading habit analysis

###  Power-Efficient Design
- Smart deep sleep mode (only 10μA power consumption)
- Adaptive wake-up intervals (1-5 minutes)
- Dual wake-up mechanism: GPIO and timer
- Intelligent I2C bus management

###  User Interaction
- 128x64 OLED display
- Pixel art style UI design
- Real-time trend graph of ambient light，dynamic wave animation effects
- Buzzer notifications for reading/rest prompts
- Real-time status indicators

##  Hardware Requirements

### Core Components
- **MCU**: ESP32-C3 / ESP32 WROOM-32D
- **Display**: SSD1306 OLED (128x64, I2C interface)
- **NFC Module**: PN532 (I2C interface)
- **Light Sensor**: Light Dependent Resistor (LDR)
- **Audio**: Active buzzer module (HYT-0905)
- **Power**: BQ24074 charging + Li-ion battery + optional solar panel

Power Options

Monocrystalline solar panel (5V) → sustainable indoor charging

USB Type-C → backup fast charging

Battery: 401020 Li-ion (~120 mAh)

Earlier prototypes attempted OPV flexible solar panels for true bookmark size, but lab tests showed insufficient indoor output (<6 mW @ 500 lux). This led to the shift toward the bookstand form factor with hybrid solar + USB charging.

### Wiring Diagram

```
ESP32 WROOM-32D:
├── OLED SSD1306 (128x64)
│   ├── SDA → GPIO 21
│   ├── SCL → GPIO 22
│   ├── VCC → 3.3V
│   └── GND → GND
├── PN532 NFC Module
│   ├── SDA → GPIO 21 (Shared I2C bus with OLED)
│   ├── SCL → GPIO 22 (Shared I2C bus with OLED)
│   ├── VCC → 3.3V
│   └── GND → GND
├── Light Dependent Resistor (LDR)
│   ├── One end → GPIO 36 (ADC input)
│   ├── Other end → 3.3V
│   └── Pull-down resistor 10kΩ → GND
├── Buzzer Module
│   ├── VCC → 3.3V
│   ├── GND → GND
│   └── Signal → GPIO 5
└── Wake-up Button (Optional)
    ├── One end → GPIO 33
    └── Other end → GND
```

##  Quick Start

### Environment Setup

1. **Install Arduino IDE** (Recommended version 2.0+)
    -Add ESP32 board support via: https://dl.espressif.com/dl/package_esp32_index.json

3. **Add ESP32 Board Support**:
   - File → Preferences → Additional Boards Manager URLs
   - Add: `https://dl.espressif.com/dl/package_esp32_index.json`
   - Tools → Board → Boards Manager → Search "ESP32" and install

4. **Install Required Libraries**:
   ```
   - Adafruit GFX Library
   - Adafruit SSD1306
   - Adafruit PN532
   - ESP32 Preferences
   ```

### Compile and Upload

1. **Select Board**:
   - ESP32 WROOM-32D: `ESP32 Dev Module`
   - ESP32-C3: `ESP32C3 Dev Module`

2. **Configure Parameters**:
   ```
   Upload Speed: 921600
   CPU Frequency: 240MHz (ESP32) / 160MHz (ESP32-C3)
   Flash Mode: DIO
   Flash Size: 4MB
   Partition Scheme: Default 4MB
   ```

3. **Choose Version**:
   - `bookstand v3/bookstand_WROOM_withbetterNFCmanagement/` - ESP32 WROOM-32D version
   - `bookstand v3/WROOM32Ddeepsleepmode/` - Deep sleep optimized version
   - `bookstand v3/finalOLEDtest/finalOLEDv3/` - ESP32-C3 version

##  Functionality

### NFC Book Management
- Place NFC card above the device for automatic book recognition
- First-time use automatically registers new books
- Support for custom book names and information
- Independent timing and statistics for each book

### Reading Detection
- Automatic reading state detection via light sensor
- Smart filtering of ambient light interference
- Adaptive threshold adjustment
- Real-time status feedback

### Pomodoro Timer
- Automatically adjusts duration based on personal reading habits
- Audio notification when timer expires
- Smart break time suggestions
- Focus concentration statistics

### Data Statistics
- Total reading time for each book
- Average reading session duration
- Reading frequency analysis
- Number of completed Pomodoro sessions

##  Advanced Configuration

### Sensor Calibration

Modify in `bookstand_WROOM_withbetterNFCmanagement.ino`:

```cpp
// Light threshold configuration
const int baselineThreshold = 500;    // Base light threshold
const int dynamicRange = 200;         // Dynamic range
const int noiseMargin = 50;           // Noise margin

// Sleep configuration
const unsigned long idleTimeout = 30000;        // Idle timeout (milliseconds)
const unsigned long lowLightSleepDelay = 2000;  // Low light delay (milliseconds)
```

### Book Management

```cpp
#define MAX_BOOKS 10              // Maximum number of books
#define BOOK_NAME_MAX_LENGTH 20   // Maximum book name length
```

### Deep Sleep Optimization

Enable deep sleep functionality to reduce standby power consumption to 10μA:

```cpp
// In WROOM32Ddeepsleepmode version
const unsigned long wakeupInterval = 60000;  // Wake-up interval (milliseconds)
```

## 📊 Version History
| Version           | Key Features                                            | Status              |
| ----------------- | ------------------------------------------------------- | ------------------- |
| **Bookmark (v1)** | Ultra-thin OPV-powered design, BLE output               | ❌ Energy infeasible |
| **Bookstand v1**  | Dock charging, FSR sensor                               | Deprecated          |
| **Bookstand v2**  | OLED + NFC + Pomodoro + solar + Type-C                  | ✅ Exhibition-tested |
| **Bookstand v3**  | Multi-book tracking, adaptive Pomodoro, I²C arbitration | ✅ Current release   |


### v3.0 - Multi-Book Management Version
- ✅ Support for tracking up to 10 books independently
- ✅ Improved NFC management system
- ✅ Book statistics data persistence
- ✅ Smart book switching notifications

### v2.3 - Deep Sleep Optimization
- ✅ True deep sleep mode (10μA power consumption)
- ✅ Smart timer wake-up
- ✅ Adaptive wake-up intervals
- ✅ GPIO wake-up support

### v2.0 - NFC Multi-Book Support
- ✅ NFC card book recognition
- ✅ Independent timing for multiple books
- ✅ OLED pixel art interface
- ✅ I2C bus conflict optimization

## User Study & Feedback

As part of project evaluation, user questionnaires were conducted during an exhibition:

Positive response to reviving obsolete but aesthetically unique objects (e.g., bookmarks, cassette players) with IoT intelligence.

Users preferred standalone functionality (OLED display, buzzer feedback) over BLE-only smartphone integration.

Bookstand v2 was rated the most popular design:

Artistic shell design

Simple & intuitive UI

Reliable NFC book switching

## Research Context

This project originated as a MSc Dissertation at UCL CASA (Connected Environments).

Explores IoT-enablement of traditional reading tools

Tests feasibility of solar-assisted embedded systems in micro form factors

Investigates balance between traditional aesthetics and smart functionality

Informs future IoT retrofits for legacy objects

Related literature includes indoor photovoltaics, low-power embedded system design, and user acceptance of IoT-enabled traditional objects.

## 🐛 Troubleshooting

### Common Issues

**Q: OLED display not working**
- Check I2C connection wires are correct
- Confirm OLED address is 0x3C
- Check power supply is normal

**Q: NFC cannot read cards**
- Confirm PN532 module I2C mode jumper settings
- Check for I2C address conflicts with OLED
- Try different types of NFC cards

**Q: Light detection inaccurate**
- Adjust LDR position to avoid obstruction
- Modify light threshold parameters in code
- Confirm pull-down resistor value is 10kΩ

**Q: Cannot wake up from deep sleep**
- Check GPIO33 pin connection
- Confirm pull-up resistor configuration is correct
- Verify deep sleep wake-up source settings

### Debug Mode

Enable serial debug information:
```cpp
Serial.begin(115200);
// Add debug output in setup()
Serial.println("=== Debug Mode Enabled ===");
```

##  Contributing

Issues and Pull Requests are welcome!

### Development Environment
- Arduino IDE 2.0+
- ESP32 Arduino Core 2.0+
- Git version control

### Commit Convention
- Feature improvements: `feat: add new feature description`
- Bug fixes: `fix: fix issue description`
- Documentation updates: `docs: update documentation content`

##  License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

##  Author

**Zhiyu Cao** - Embedded developer and reading enthusiast

##  Acknowledgments

- Adafruit - Excellent hardware library support
- ESP32 Community - Rich development resources
- All testers for valuable feedback

---

**📖 Enjoy smart reading time! If this project helps you, please give us a ⭐️**
