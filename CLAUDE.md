# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with the WeatherSensor application.

## Project Overview

**WeatherSensor** is a battery-powered environmental monitoring system built on the Adafruit Feather ESP32-S2 TFT board running Zephyr RTOS 4.4-rc1. The application collects temperature, pressure, humidity, and air quality data from external SPI sensors, displays them locally on the TFT screen, and transmits batched data to a remote server.

**Key Requirements:**
- Low power consumption (Li-Ion battery powered)
- Deep sleep modes between sensor readings
- Data buffering for batch transmission
- Local display of current readings
- Battery level monitoring
- Robust error handling and recovery

**Current Development Stage:**
Initial development - basic time display with UART console logging. Sensors, networking, and power management will be added incrementally.

## Hardware Platform

**Board:** Adafruit Feather ESP32-S2 **Reverse** TFT ⚠️
- **MCU:** ESP32-S2 (Xtensa LX7 single-core @ 240MHz)
- **RAM:** 320 KB SRAM
- **Display:** Built-in 1.14" TFT, 135x240 pixels (ST7789V controller)
- **Connectivity:** Wi-Fi 802.11 b/g/n (no Bluetooth)
- **Power:** Li-Ion battery support with charging circuit
- **Sensors (future):** Temperature/Pressure/Humidity/Air Quality via SPI
- **Debug:** UART console via **GPIO43/44 (DB pin)** - NOT GPIO1/2!
  - **CRITICAL:** GPIO1/GPIO2 are BUTTONS on Reverse board, not UART!
  - **IMPORTANT:** USB-CDC is NOT supported in Zephyr for ESP32-S2
  - USB-C port only provides power and flashing capability
  - Console logs require external USB-to-UART adapter (ESP-Prog recommended)

## Project Structure

```
WeatherSensor/
├── CLAUDE.md              # This file - AI assistant guidance
├── CMakeLists.txt         # Build configuration
├── prj.conf               # Kconfig settings
├── docs/                  # Documentation
│   ├── ESP-PROG-GUIDE.md # ESP-Prog hardware setup and usage
│   └── HARDWARE.md       # Hardware architecture guide (START HERE!)
├── boards/                # Device tree overlays
│   └── adafruit_feather_esp32s2_tft_reverse.overlay  # Reverse board config
├── src/                   # Source files
│   ├── main.cpp          # Application entry point (LVGL version - future)
│   ├── main_simple.cpp   # Simple display test (CURRENT, well-documented)
│   ├── display/          # Display management (future)
│   ├── sensors/          # Sensor drivers (future)
│   ├── network/          # Server communication (future)
│   └── power/            # Power management (future)
└── inc/                   # Header files
```

## Build Commands

### Complete Environment Setup

**IMPORTANT:** Always follow these steps in order before building:

```bash
# 1. Navigate to workspace root
cd /Users/lukaszronka/projects/zephyr-4.4

# 2. Activate Python virtual environment (required for west and dependencies)
# SDK path and Zephyr base are configured in .west/config - no extra sourcing needed
source ./.venv/bin/activate
```

### Building and Flashing

```bash
# Build WeatherSensor for ESP32-S2 Reverse TFT Feather
west build -p always -b adafruit_feather_esp32s2_tft_reverse WeatherSensor

# Flash to device (USB-C connected)
west flash --esp-device /dev/cu.usbmodem01  # macOS
# OR
west flash  # Auto-detect (may scan multiple ports)

# Monitor UART console output
# NOTE: Requires ESP-Prog or USB-to-UART adapter
# IMPORTANT: On Reverse board, console is GPIO43/44 (DB pin), NOT GPIO1/GPIO2!
# ESP-Prog connections (note: ESP-Prog has internal TX/RX crossover):
#   - ESP-Prog TXD0 -> ESP32-S2 GPIO43 (DB pin, TX)
#   - ESP-Prog RXD0 -> ESP32-S2 GPIO44 (RX)
#   - ESP-Prog GND  -> ESP32-S2 GND
# Default baud rate: 115200

# Monitor with screen (macOS - use cu.* not tty.*)
screen /dev/cu.usbserial-1101 115200  # ESP-Prog UART channel

# Exit screen: Ctrl+A then K then Y
```

**See docs/ESP-PROG-GUIDE.md for complete wiring and setup instructions!**

### Quick Setup Alias (Optional)

Add this to your `~/.zshrc` or `~/.bashrc` for convenience:

```bash
alias zephyr-setup='cd /Users/lukaszronka/projects/zephyr-4.4 && source ./.venv/bin/activate'
```

Then simply run:
```bash
zephyr-setup
west build -p always -b adafruit_feather_esp32s2_tft_reverse WeatherSensor
```

### Verifying Environment

Check that the environment is set up correctly:

```bash
# Check Python venv is active (should show venv path)
which python

# Check west is available (should show version)
west --version

# Check SDK is configured (should show path in .west/config)
west config build.cmake-args
```

## Architecture Design

### Multi-Threading Pattern

The application will use Zephyr's multi-threading with dedicated threads for different subsystems:

1. **Sensor Thread** - Periodic sensor data collection from SPI devices
2. **Display Thread** - UI updates and user interaction handling
3. **Network Thread** - Batch data transmission to server
4. **Power Management Thread** - Battery monitoring, deep sleep coordination
5. **Main Thread** - System coordination and command processing

**Thread Communication:**
- Message queues for sensor data
- Work queues for event-driven tasks
- Semaphores for resource synchronization
- Shared ring buffers for data buffering

### Power Management Strategy

- **Active Mode:** All threads running, frequent sensor reads
- **Idle Mode:** Reduced sampling rate, display dimmed
- **Deep Sleep:** Only RTC and wakeup sources active
- **Wake Sources:** Timer (periodic), button press, low battery alert

### Data Flow

```
Sensors (SPI) → Sensor Thread → Ring Buffer → Network Thread → Server
                       ↓
                 Display Thread → TFT Screen
                       ↓
              Power Mgmt Thread → Battery Monitor
```

## Configuration

### Key Kconfig Options (prj.conf)

```
CONFIG_CPP=y                    # C++ support
CONFIG_STD_CPP20=y             # C++20 standard
CONFIG_SPI=y                   # SPI for sensors (future)
CONFIG_DISPLAY=y               # Display support
CONFIG_LVGL=y                  # LVGL GUI library
CONFIG_WIFI=y                  # Wi-Fi connectivity (future)
CONFIG_PM=y                    # Power management (future)
CONFIG_PM_DEVICE=y             # Device power management (future)
CONFIG_LOG=y                   # Logging framework
CONFIG_UART_CONSOLE=y          # UART console output
```

### Device Tree Configuration

Hardware configuration in `boards/adafruit_feather_esp32s2_tft_reverse.overlay`:
- UART0 on GPIO43/44 (DB pin) for console output
- SPI2 bus for TFT display
- ST7789V TFT display controller
- I2C for future sensor support
- GPIO7 for display power control
- GPIO45 for backlight control

## Development Phases

### Phase 1: Basic Setup ✅ COMPLETE
- [x] Project structure
- [x] UART console logging (via ESP-Prog on GPIO43/44)
- [x] Display power control (GPIO7)
- [x] Backlight control (GPIO45)
- [x] Display test pattern (colored bars and cycling colors)
- [x] Clean, well-documented code
- [ ] Basic LVGL UI (next step)

### Phase 2: Sensor Integration
- [ ] SPI driver configuration
- [ ] Temperature/Pressure sensor (BME280/BMP280)
- [ ] Humidity sensor
- [ ] Air quality sensor
- [ ] Sensor thread implementation
- [ ] Data buffering

### Phase 3: Display & UI
- [ ] LVGL interface design
- [ ] Real-time sensor display
- [ ] Battery level indicator
- [ ] Status icons
- [ ] Display thread with work queue

### Phase 4: Networking
- [ ] Wi-Fi connection management
- [ ] HTTP/MQTT client
- [ ] Batch data transmission
- [ ] Network thread implementation
- [ ] Retry logic and error handling

### Phase 5: Power Management
- [ ] Battery voltage monitoring
- [ ] Deep sleep implementation
- [ ] Wake-up source configuration
- [ ] Dynamic power mode switching
- [ ] Power management thread

### Phase 6: Integration & Optimization
- [ ] Thread pool optimization
- [ ] Memory usage optimization
- [ ] Power consumption testing
- [ ] Field testing

## Coding Style

Following Zephyr RTOS conventions:

- **Style:** Linux kernel coding style
- **Line Length:** 100 characters maximum
- **Comments:** C89-style `/* */` for general, Doxygen `/** */` for APIs
- **Naming:**
  - `snake_case` for functions and variables
  - `PascalCase` for C++ classes
  - `UPPER_CASE` for macros and constants
- **Braces:** Always use, even for single-line blocks
- **C++ Standard:** C++20 features allowed

## Key Zephyr APIs

### Threading
```c
K_THREAD_DEFINE(thread_name, stack_size, entry_func, p1, p2, p3, priority, options, delay);
k_thread_create(&thread, stack, stack_size, entry, p1, p2, p3, priority, options, delay);
k_msleep(ms);
k_yield();
```

### Synchronization
```c
k_sem_init(&sem, initial, limit);
k_sem_take(&sem, timeout);
k_sem_give(&sem);
k_mutex_lock(&mutex, timeout);
k_msgq_put(&msgq, data, timeout);
```

### Power Management
```c
pm_device_action_run(dev, PM_DEVICE_ACTION_SUSPEND);
pm_state_force(0u, &(struct pm_state_info){PM_STATE_SUSPEND_TO_IDLE, 0, 0});
```

### Logging
```c
LOG_MODULE_REGISTER(module_name, LOG_LEVEL_DBG);
LOG_INF("Info message");
LOG_ERR("Error: %d", error_code);
```

### Device Access
```c
const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(spi0));
device_is_ready(dev);
```

## Testing Strategy

- **Unit Tests:** Twister framework for individual components
- **Integration Tests:** End-to-end data flow validation
- **Power Tests:** Battery life measurement with profiler
- **Stress Tests:** Extended runtime with error injection

## Debugging

### Console Logging (UART)

**Hardware Required**: ESP-Prog or USB-to-UART adapter

Console logs are available via GPIO1 (TX) and GPIO2 (RX) at 115200 baud.

**Quick Start with ESP-Prog**:
```bash
# Connect to console (see docs/ESP-PROG-GUIDE.md for wiring)
screen /dev/cu.usbserial-1420 115200

# Exit: Ctrl+A, then K, then Y
```

**See docs/ESP-PROG-GUIDE.md for complete setup instructions!**

### JTAG Debugging (Breakpoints, Step-through)

**Hardware Required**: ESP-Prog with JTAG connected

```bash
# Build with debug symbols
west build -b adafruit_feather_esp32s2_tft_reverse WeatherSensor -- -DCMAKE_BUILD_TYPE=Debug

# Flash firmware
west flash --esp-device /dev/cu.usbmodem01

# Start GDB debugging session
west debug
```

**See docs/ESP-PROG-GUIDE.md for JTAG connection details and GDB commands!**

## Understanding the Codebase

### Current Application: Simple Display Test

The `src/main_simple.cpp` is a clean, well-documented reference implementation demonstrating:

1. **GPIO Control** - LED for status, display power (GPIO7), backlight (GPIO45)
2. **Display Buffer Management** - Memory-efficient strip drawing technique
3. **Zephyr Device Model** - Device tree macros, GPIO specs, device access
4. **Critical Initialization Sequence** - Power-on order that must be followed

**Read these files to understand the system:**
- **`docs/HARDWARE.md`** - ⭐ **START HERE!** Complete hardware architecture guide
- `src/main_simple.cpp` - Cleaned, well-documented application code
- `boards/adafruit_feather_esp32s2_tft_reverse.overlay` - Hardware configuration
- `docs/ESP-PROG-GUIDE.md` - Debug console and JTAG setup

### Key Concepts to Master

#### 1. Device Tree
Hardware description language used by Zephyr to define what hardware exists and how it's connected. Compiled into application at build time, modified via overlay files for board-specific changes.

```c
/* Example: Get GPIO from device tree */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
```

#### 2. Critical Initialization Order
**GPIO7 (display power) MUST be HIGH before accessing display!**

```c
init_display_power();  // GPIO7 HIGH
k_msleep(10);          // Wait for power rail to stabilize
init_display();        // Now safe to access ST7789V
init_backlight();      // Can happen anytime after display ready
```

**Why manual control?** We tested Zephyr's power domain subsystem (CONFIG_PM)
but it causes boot crashes on this board. Manual GPIO control is the proven,
reliable approach.

#### 3. Memory-Constrained Drawing
Full screen buffer (64KB) won't fit in 32KB heap. Solution: draw in horizontal strips.

```c
// BAD: Allocates 64KB (fails)
fill_rect(0, 0, 135, 240, color);

// GOOD: Allocates 8KB per strip
for (int y = 0; y < 240; y += 30) {
    fill_rect(0, y, 135, 30, color);
}
```

#### 4. RGB565 Color Format
16-bit color format used directly by ST7789V hardware:
- 5 bits red, 6 bits green, 5 bits blue
- `0xF800` = red, `0x07E0` = green, `0x001F` = blue
- No conversion needed, sent directly to display

#### 5. Reverse Board Differences

**Note:** This project supports the **Reverse TFT** only. The table below shows key differences for reference.

| Feature | Regular TFT | Reverse TFT (This Project) |
|---------|-------------|----------------------------|
| Console UART | GPIO1/2 | **GPIO43/44 (DB pin)** |
| GPIO1/GPIO2 | UART | **Buttons!** |
| Board Target | `..._tft` | `..._tft_reverse` |

## Resources

- **Hardware Guide:** `docs/HARDWARE.md` - ⭐ **START HERE!**
- **ESP-Prog Setup:** `docs/ESP-PROG-GUIDE.md` - Debug console and JTAG
- **Zephyr Docs:** https://docs.zephyrproject.org/latest/
- **ESP32-S2 Docs:** https://docs.espressif.com/projects/esp-idf/en/latest/esp32s2/
- **Board Support:** https://docs.zephyrproject.org/latest/boards/espressif/index.html
- **Display API:** https://docs.zephyrproject.org/latest/hardware/peripherals/display.html
- **GPIO API:** https://docs.zephyrproject.org/latest/hardware/peripherals/gpio.html
- **Device Tree:** https://docs.zephyrproject.org/latest/build/dts/index.html
- **LVGL Docs:** https://docs.lvgl.io/
- **Power Management:** https://docs.zephyrproject.org/latest/services/pm/index.html

## Notes

### Critical Information

- **Board Target:** Always use `adafruit_feather_esp32s2_tft_reverse` (NOT `..._tft`)
- **Console UART:** GPIO43/44 (DB pin), NOT GPIO1/GPIO2 (those are buttons on Reverse!)
- **Display Power:** GPIO7 MUST be HIGH before initializing display
- **USB Limitation:** USB-CDC is NOT supported in Zephyr for ESP32-S2
- **Flashing:** USB-C port works for flashing firmware via `west flash`
- **Console Logging:** Requires ESP-Prog or USB-to-UART adapter on GPIO43/44 at 115200 baud
- **macOS Serial:** Use `/dev/cu.*` devices, NOT `/dev/tty.*` (avoids "resource busy")

### Environment Setup

- Always activate Python venv before building (SDK and Zephyr base are configured in .west/config)
- See **Build Commands** section for complete setup sequence

### Hardware Details

- **MCU:** ESP32-S2 is single-core (no dual-core threading like S3)
- **Connectivity:** Wi-Fi only (no Bluetooth support)
- **RAM:** 320 KB SRAM total, 32 KB heap configured
- **Heap Usage:** Full screen buffer (64KB) won't fit - use strip drawing
- **Display:** 135x240 RGB565, ST7789V controller via SPI2

### Power Management

- **Display Power (GPIO7):** Manual GPIO control (PM subsystem causes boot crashes)
- **Backlight (GPIO45):** Manual GPIO control (simple on/off)
- **Power Domain Test:** Attempted but failed - causes silent boot crashes
- **Future:** Deep sleep will require wake-up timer or GPIO trigger

### Future Development

- SPI clock speed must match sensor specifications
- LVGL requires sufficient heap (`CONFIG_HEAP_MEM_POOL_SIZE`)
- Wi-Fi networking integration
