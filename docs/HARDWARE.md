# Hardware Architecture Guide

This document explains the hardware architecture and key concepts for the WeatherSensor application on the ESP32-S2 Reverse TFT Feather board.

## Table of Contents

1. [Board Overview](#board-overview)
2. [Key Hardware Components](#key-hardware-components)
3. [GPIO Pin Mapping](#gpio-pin-mapping)
4. [Display Architecture](#display-architecture)
5. [Memory Management](#memory-management)
6. [Zephyr Device Model](#zephyr-device-model)
7. [Critical Initialization Sequence](#critical-initialization-sequence)
8. [Power Management](#power-management)

---

## Board Overview

**Board:** Adafruit Feather ESP32-S2 Reverse TFT
**MCU:** ESP32-S2 (Xtensa LX7 single-core @ 240MHz)
**RAM:** 320 KB SRAM
**Flash:** 4 MB
**Display:** 1.14" TFT, 135x240 pixels, ST7789V controller
**Connectivity:** Wi-Fi 802.11 b/g/n (2.4 GHz only, no Bluetooth)

### Key Differences from Regular TFT Feather

**This project supports the Reverse TFT ONLY.** The table below shows critical differences from the regular TFT variant (for reference).

| Feature | Regular TFT | Reverse TFT (This Project) |
|---------|-------------|----------------------------|
| GPIO1/GPIO2 | UART TX/RX | **BUTTONS** (not UART!) |
| Console UART | UART1 (GPIO1/2) | **UART0 (GPIO43/44)** |
| Debug Pin | None | **DB pin (GPIO43)** - dedicated UART TX |
| Display Orientation | Standard | Reversed (screen upside down) |

**Board target:** `adafruit_feather_esp32s2_tft_reverse` (the only one in this project).

---

## Key Hardware Components

### 1. Status LED (GPIO13)

- **Purpose:** Visual feedback for system status
- **Location:** On-board red LED
- **Usage:**
  - 3 quick blinks on boot = successful initialization
  - Slow blink (1Hz) = normal operation (heartbeat)
  - Fast blink (5Hz) = error state

### 2. Display (ST7789V via SPI2)

- **Controller:** ST7789V (16-bit color TFT)
- **Resolution:** 135 x 240 pixels
- **Interface:** SPI2 (high-speed serial)
- **Pixel Format:** RGB565 (16 bits per pixel, 5:6:5 bit distribution)
- **Buffer Size:** Full screen = 135 × 240 × 2 = 64,800 bytes

### 3. Display Power (GPIO7)

- **Critical:** This GPIO controls power to the display
- **Must be HIGH before accessing display controller**
- **Without this:** Display will not respond to any commands
- **Sequence:** Enable GPIO7 → wait 10ms → initialize display

### 4. Backlight (GPIO45)

- **Purpose:** Controls TFT backlight brightness (on/off only)
- **Independent:** Can be controlled separately from display controller
- **Usage:** Turn off for power saving, turn on for visibility

### 5. Console UART (UART0 on GPIO43/44)

- **TX:** GPIO43 (DB pin - debug output only)
- **RX:** GPIO44 (optional, not used in simple test)
- **Baud Rate:** 115200 8N1
- **Note:** ESP32-S2 does NOT support USB-CDC in Zephyr - requires external adapter

---

## GPIO Pin Mapping

### Display & Control GPIOs

| GPIO | Function | Direction | Notes |
|------|----------|-----------|-------|
| GPIO7 | Display Power | Output | Must enable before display init |
| GPIO13 | Status LED | Output | On-board red LED |
| GPIO45 | Backlight | Output | TFT backlight control |

### UART Console

| GPIO | Function | Direction | Notes |
|------|----------|-----------|-------|
| GPIO43 | UART0 TX (DB pin) | Output | Console output, connects to ESP-Prog TXD0 |
| GPIO44 | UART0 RX | Input | Console input (optional) |

### SPI2 (Display)

| GPIO | Function | Direction | Notes |
|------|----------|-----------|-------|
| GPIO36 | MOSI | Output | SPI data to display |
| GPIO35 | SCK | Output | SPI clock |
| GPIO37 | CS | Output | Chip select (active low) |
| GPIO39 | DC | Output | Data/Command select |

### Buttons (Reverse Board)

| GPIO | Function | Notes |
|------|----------|-------|
| GPIO1 | Button A | **NOT UART!** (This is a button on Reverse board) |
| GPIO2 | Button B | **NOT UART!** |

---

## Display Architecture

### ST7789V Controller

The display uses the ST7789V controller, a popular 16-bit color TFT driver.

**Communication Flow:**
```
ESP32-S2 → SPI2 → ST7789V → TFT Panel → Your Eyes
```

**Key Concepts:**

1. **RGB565 Color Format**
   - 16 bits per pixel: `RRRR RGGG GGGB BBBB`
   - Red: 5 bits (32 levels)
   - Green: 6 bits (64 levels) - human eye more sensitive to green
   - Blue: 5 bits (32 levels)
   - Example: `0xF800` = pure red, `0x07E0` = pure green, `0x001F` = pure blue

2. **Display Buffer Descriptor**
   ```c
   struct display_buffer_descriptor {
       size_t buf_size;  // Total buffer size in bytes
       uint16_t width;   // Width in pixels
       uint16_t height;  // Height in pixels
       uint16_t pitch;   // Row stride (usually = width)
   };
   ```

3. **Display Write Operation**
   ```c
   display_write(display_dev, x, y, &buf_desc, buffer);
   ```
   - `x, y`: Top-left corner coordinates
   - `buf_desc`: Buffer descriptor (size, dimensions)
   - `buffer`: Pointer to pixel data (RGB565 format)

### Display Blanking

- **Blanking ON:** Display shows black (controller still powered)
- **Blanking OFF:** Display shows content
- Called during initialization: `display_blanking_off(display_dev)`

---

## Memory Management

### ESP32-S2 Memory Layout

- **Total SRAM:** 320 KB
- **Heap Pool:** 32 KB (configured via `CONFIG_HEAP_MEM_POOL_SIZE`)
- **Stack:** 4 KB main stack (configured via `CONFIG_MAIN_STACK_SIZE`)
- **Used by:** Zephyr kernel, drivers, application, buffers

### Display Buffer Strategy

**Problem:** Full screen buffer is too large!

```
Full screen: 135 × 240 pixels × 2 bytes/pixel = 64,800 bytes
Heap size: 32,768 bytes
Result: ALLOCATION FAILS ❌
```

**Solution:** Draw in horizontal strips

```c
#define STRIP_HEIGHT 30  // 30 pixels high

for (int y = 0; y < screen_height; y += STRIP_HEIGHT) {
    uint16_t h = MIN(STRIP_HEIGHT, screen_height - y);
    // Allocate only: 135 × 30 × 2 = 8,100 bytes ✓
    fill_rect(0, y, screen_width, h, color);
}
```

**Benefits:**
- Each strip is only 8,100 bytes (fits in 32KB heap)
- Total memory used is reused for each strip
- Allows full-screen drawing without increasing heap size

### Memory Allocation APIs

```c
/* Zephyr heap allocation (used in fill_rect) */
void *k_malloc(size_t size);
void k_free(void *ptr);

/* C standard library (requires CONFIG_HEAP_MEM_POOL_SIZE) */
void *malloc(size_t size);
void free(void *ptr);
```

**Important:** `k_malloc()` allocates from the Zephyr heap configured by `CONFIG_HEAP_MEM_POOL_SIZE`.

---

## Zephyr Device Model

### Device Tree Basics

Zephyr uses **Device Tree** (DTS) to describe hardware. Think of it as a blueprint that tells Zephyr what hardware exists and how it's connected.

**Key Device Tree Files:**
1. **Base Board DTS:** `zephyr/boards/espressif/adafruit_feather_esp32s2_tft_reverse/...dts` (in Zephyr source)
2. **Project Overlay:** `boards/adafruit_feather_esp32s2_tft_reverse.overlay` (your customizations)
3. **Compiled:** `.dts` + overlay → Device Tree Compiler → `.dtb` (binary)

### Device Tree Macros

These macros retrieve device/GPIO information at compile time:

```c
/* Get GPIO from alias */
GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios)
// Expands to: { .port = ..., .pin = 13, .dt_flags = GPIO_ACTIVE_LOW }

/* Get device from chosen node */
DEVICE_DT_GET(DT_CHOSEN(zephyr_display))
// Returns: pointer to display device driver

/* Get GPIO by index from a node */
GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(i2c_reg), enable_gpios, 0)
// Gets first GPIO from enable-gpios property of /i2c_reg node
```

### Device Initialization

Zephyr automatically initializes devices based on device tree. The driver's `init()` function runs at boot before `main()`.

**Checking Device Readiness:**
```c
const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
if (!device_is_ready(dev)) {
    // Device failed to initialize or doesn't exist
}
```

### GPIO Device Tree Spec

```c
struct gpio_dt_spec {
    const struct device *port;  // GPIO controller device
    gpio_pin_t pin;             // Pin number
    gpio_dt_flags_t dt_flags;   // Flags (active high/low, pull-up/down)
};
```

**Usage Pattern:**
```c
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

if (!gpio_is_ready_dt(&led)) {
    return -ENODEV;  // GPIO not available
}

gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);  // Configure as output
gpio_pin_set_dt(&led, 1);  // Set HIGH
gpio_pin_set_dt(&led, 0);  // Set LOW
gpio_pin_toggle_dt(&led);  // Toggle state
```

---

## Critical Initialization Sequence

### The Order Matters!

**CORRECT SEQUENCE:**

```
1. init_led()
   └─> Configure GPIO13 as output
   └─> 3 quick blinks (visual feedback)

2. init_display_power()
   └─> Configure GPIO7 as output
   └─> Set GPIO7 HIGH (enable display power)
   └─> Wait 10ms for power rail to stabilize  ⚠️ CRITICAL

3. init_display()
   └─> Get display device from device tree
   └─> Zephyr driver initializes ST7789V (SPI communication)
   └─> Retrieve display capabilities (resolution, format)
   └─> Disable blanking (turn on display output)

4. init_backlight()
   └─> Configure GPIO45 as output
   └─> Set GPIO45 HIGH (turn on backlight)

5. draw_test_pattern()
   └─> Now safe to write to display!
```

### What Happens If You Skip Display Power?

```
❌ WRONG: init_display() → init_display_power() → draw()

Result:
- Display driver tries to communicate with ST7789V via SPI
- ST7789V has no power, doesn't respond
- Driver timeouts or returns errors
- Display remains black forever
```

### Why the 10ms Delay?

Power rails need time to stabilize. The ST7789V requires stable VDD before it can process commands.

```c
gpio_pin_set_dt(&display_power, 1);  // Turn on power
k_msleep(10);                        // Wait for capacitors to charge
// Now safe to access display
```

---

## Power Management

### Current Implementation: Manual Control

The application manually controls power GPIOs without using Zephyr's power management subsystem.

**Why?** The Zephyr PM subsystem caused boot crashes on this board (likely a driver compatibility issue with ESP32-S2 HAL).

**Manual Control:**
```c
/* Display power */
gpio_pin_set_dt(&display_power, 1);  // ON
gpio_pin_set_dt(&display_power, 0);  // OFF (saves ~20mA)

/* Backlight */
gpio_pin_set_dt(&backlight, 1);  // ON
gpio_pin_set_dt(&backlight, 0);  // OFF (saves ~50mA)
```

### Future: Proper Power Management

When PM is working, you would use:

```c
#include <zephyr/pm/pm.h>
#include <zephyr/pm/device.h>

/* Suspend display */
pm_device_action_run(display_dev, PM_DEVICE_ACTION_SUSPEND);

/* Resume display */
pm_device_action_run(display_dev, PM_DEVICE_ACTION_RESUME);

/* Enter sleep mode */
pm_state_force(0u, &(struct pm_state_info){PM_STATE_SUSPEND_TO_IDLE, 0, 0});
```

### Battery-Powered Operation

For the WeatherSensor application, you'll eventually implement:

1. **Active Mode:** All peripherals on, frequent sensor reads
2. **Idle Mode:** Display dimmed, reduced sampling rate
3. **Deep Sleep:** CPU off, RTC running, wake on timer
4. **Wake Sources:**
   - RTC timer (periodic sensor readings)
   - Button press (GPIO interrupt)
   - Low battery alert

---

## Key Takeaways

### 1. Always Enable Display Power First
```c
init_display_power();  // GPIO7 HIGH
k_msleep(10);          // Stabilize
init_display();        // Now safe
```

### 2. Draw in Strips for Large Areas
```c
// BAD: Allocates 64KB (fails)
fill_rect(0, 0, 135, 240, color);

// GOOD: Allocates 8KB per strip
for (int y = 0; y < 240; y += 30) {
    fill_rect(0, y, 135, 30, color);
}
```

### 3. Use Device Tree Macros
```c
// Gets compile-time device info
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
```

### 4. Check Device Readiness
```c
if (!device_is_ready(display_dev)) {
    // Handle error
}
```

### 5. Reverse Board Is Different
- Console is UART0 (GPIO43/44), not UART1 (GPIO1/2)
- GPIO1/GPIO2 are buttons, not UART
- Always use correct board target

---

## Additional Resources

- **ST7789V Datasheet:** Display controller specifications
- **ESP32-S2 Technical Reference:** https://www.espressif.com/en/support/documents/technical-documents
- **Zephyr Display API:** https://docs.zephyrproject.org/latest/hardware/peripherals/display.html
- **Zephyr GPIO API:** https://docs.zephyrproject.org/latest/hardware/peripherals/gpio.html
- **Device Tree Guide:** https://docs.zephyrproject.org/latest/build/dts/index.html
- **ESP-PROG Setup:** See `docs/ESP-PROG-GUIDE.md`

---

**Last Updated:** 2026-03-02
**Author:** Lukasz Ronka
