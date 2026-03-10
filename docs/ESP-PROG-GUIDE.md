# ESP-Prog Setup Guide for ESP32-S2 Reverse TFT Feather

**Complete guide for using ESP-Prog with Adafruit ESP32-S2 Reverse TFT Feather board.**

⚠️ **IMPORTANT:** This guide is specifically for the **REVERSE** TFT variant. The Reverse board has different pin assignments than the regular TFT board!

## Table of Contents

1. [Overview](#overview)
2. [Critical Information for Reverse Board](#critical-information-for-reverse-board)
3. [Hardware Setup - UART (Console Logging)](#hardware-setup---uart-console-logging)
4. [Driver Installation (macOS)](#driver-installation-macos)
5. [Using UART Console](#using-uart-console)
6. [Hardware Setup - JTAG (Advanced)](#hardware-setup---jtag-advanced)
7. [Using JTAG Debugging](#using-jtag-debugging)
8. [Troubleshooting](#troubleshooting)
9. [Quick Reference](#quick-reference)

---

## Overview

### What is ESP-Prog?

ESP-Prog is Espressif's official 2-in-1 development tool:
- **UART adapter** for console logging (like FTDI FT232RL)
- **JTAG debugger** for breakpoints, stepping, variable inspection

### ESP-Prog Features

- **Chip**: FTDI FT2232HL (dual-channel USB-to-serial)
- **Two functions**: PROGRAM (UART) and JTAG
- **Both can be used simultaneously**
- **Price at Botland.com.pl**: 99.90 PLN

### What You'll Need

- ✅ ESP-Prog board
- ✅ Jumper wires (female-to-female, 3 minimum for UART)
- ✅ USB-A to USB-A cable (for ESP-Prog)
- ✅ USB-C cable (for ESP32-S2 power and flashing)
- ⚠️ Soldering iron (optional, for JTAG access)

---

## Critical Information for Reverse Board

### ⚠️ The Reverse Board is Different!

**This project is configured for Reverse TFT ONLY.** The table below shows key differences from the regular TFT variant (for reference).

| Feature | Regular TFT | **Reverse TFT (This Project)** |
|---------|-------------|--------------------------------|
| Console UART | GPIO1/GPIO2 | **GPIO43/GPIO44 (DB pin)** ⚠️ |
| GPIO1/GPIO2 | UART TX/RX | **BUTTONS!** (NOT UART!) |
| Board Target | `..._tft` | `..._tft_reverse` |
| Screen Orientation | Standard | Reversed (upside down) |

### Key Points:

1. **GPIO1 and GPIO2 are BUTTONS on Reverse board!** They are NOT UART pins!
2. **Console UART is on GPIO43/GPIO44** (exposed via DB pin)
3. **DB pin is TX-only** but sufficient for seeing console logs
4. **Board target:** `adafruit_feather_esp32s2_tft_reverse` (the only one supported in this project)

---

## Hardware Setup - UART (Console Logging)

### Understanding ESP-Prog's Internal Crossover

🔴 **CRITICAL DIFFERENCE:** ESP-Prog has **internal TX/RX crossover!**

This means you connect:
- **ESP-Prog ESP_TXD (pin 3) → ESP32-S2 TX (GPIO43)** (NOT crossed!)
- **ESP-Prog ESP_RXD (pin 5) → ESP32-S2 RX (GPIO44)** (NOT crossed!)

**This is opposite of normal UART adapters!** Most USB-UART adapters require you to cross TX/RX, but ESP-Prog has this done internally.

**Why?** The ESP-Prog pins are labeled from the ESP32's perspective:
- **ESP_TXD** = "ESP transmits data" = connects to ESP32's TX pin
- **ESP_RXD** = "ESP receives data" = connects to ESP32's RX pin

### ESP-Prog PROGRAM Port Pinout

```
ESP-Prog Board (top view):
┌──────────────────────────────┐
│                              │
│   [JTAG]      [PROGRAM]      │  ← Two 6-pin headers
│                              │
│       [USB Port]             │
└──────────────────────────────┘

PROGRAM Connector (2x3 pins):
┌─────────┬─────────┐
│ ESP_EN  │ VDD     │  Pin 1, 2
│ ESP_TXD │ GND     │  Pin 3, 4  ← We use pins 3 & 4
│ ESP_RXD │ ESP_IO0 │  Pin 5, 6  ← We use pin 5
└─────────┴─────────┘

Visual pin numbering (looking at connector):
     ┌─────┬─────┐
     │  1  │  2  │  ← Top row    (ESP_EN, VDD)
     │  3  │  4  │  ← Middle row (ESP_TXD, GND) ✓ CONNECT THESE
     │  5  │  6  │  ← Bottom row (ESP_RXD, ESP_IO0) ✓ CONNECT PIN 5
     └─────┴─────┘

Connect jumper wires to:
  Pin 3: ESP_TXD  → to ESP32-S2 GPIO43 (DB pin)
  Pin 4: GND      → to ESP32-S2 GND
  Pin 5: ESP_RXD  → to ESP32-S2 GPIO44 (optional)
```

**Pins we'll use:**
- **Pin 4 (GND)** - Connect to ESP32-S2 GND
- **Pin 3 (ESP_TXD)** - Connect to ESP32-S2 GPIO43 (DB pin, TX)
- **Pin 5 (ESP_RXD)** - Connect to ESP32-S2 GPIO44 (RX) [optional]

**Pins we WON'T use:**
- **Pin 2 (VDD)** (3.3V) - DON'T CONNECT (ESP32-S2 has its own power via USB-C)
- **Pin 1 (ESP_EN)** - Auto-reset, not needed
- **Pin 6 (ESP_IO0)** - Bootloader mode, not needed

### Locating the DB Pin on Reverse Board

The **DB pin** is a special debug output pin on the Adafruit Reverse TFT:
- **Function**: UART0 TX (GPIO43) - hardware debug output
- **Location**: Labeled **"DB"** on the Feather side headers
- **Purpose**: Provides ESP-IDF/ROM bootloader logs + application console

### Wiring Diagram - Correct Connections

```
ESP-Prog PROGRAM Port            ESP32-S2 Reverse TFT
┌───────────────────┐           ┌──────────────────────┐
│ Pin Layout:       │           │                      │
│  1  2  (EN, VDD)  │           │                      │
│  3  4  (TX, GND)  │           │                      │
│  5  6  (RX, IO0)  │           │                      │
│                   │           │                      │
│ Pin 4 (GND) ──────┼──────────►│ GND                  │
│                   │           │                      │
│ Pin 3 (ESP_TXD) ──┼──────────►│ GPIO43 (DB pin, TX)  │ ← NOT CROSSED!
│                   │           │                      │
│ Pin 5 (ESP_RXD) ──┼──────────►│ GPIO44 (RX)          │ ← Optional
│                   │           │                      │
│ Pin 2 (VDD)       │  ⚠️ NC    │                      │
│ Pin 1 (ESP_EN)    │  ⚠️ NC    │                      │
│ Pin 6 (ESP_IO0)   │  ⚠️ NC    │                      │
│                   │           │                      │
│ USB to Mac        │           │ USB-C to Mac         │
└───────────────────┘           └──────────────────────┘

IMPORTANT: ESP-Prog has internal crossover!
           ESP_TXD (pin 3) goes to TX (GPIO43)
           ESP_RXD (pin 5) goes to RX (GPIO44)
```

### Physical Connection Steps

1. **Identify the DB pin** on your ESP32-S2 Reverse TFT:
   - Look for "DB" label on the Feather side headers
   - This is GPIO43 - the hardware UART0 TX output
   - Reference: [Adafruit Reverse TFT Pinout](https://learn.adafruit.com/esp32-s2-reverse-tft-feather/pinouts)

2. **Connect 3 wires** (or 2 minimum):
   ```
   ESP-Prog PROGRAM      →    ESP32-S2 Reverse TFT
   ─────────────────────────────────────────────────
   Pin 4 (GND)           →    GND
   Pin 3 (ESP_TXD)       →    GPIO43 (DB pin, TX)  ⚠️ NOT CROSSED!
   Pin 5 (ESP_RXD)       →    GPIO44 (RX)          ⚠️ Optional
   ```

3. **Triple-check your connections:**
   - ✅ **Pin 3 (ESP_TXD)** goes to **GPIO43 (DB pin)** - NOT to GPIO1!
   - ✅ **Pin 5 (ESP_RXD)** goes to **GPIO44** (if you want bidirectional) - NOT to GPIO2!
   - ✅ **Pin 4 (GND)** to GND
   - ❌ **Pin 2 (VDD)** NOT connected (ESP32-S2 powered via USB-C)
   - ❌ **Pin 1 (ESP_EN)** NOT connected (auto-reset not needed)
   - ❌ **Pin 6 (ESP_IO0)** NOT connected (bootloader mode not needed)
   - ❌ DO NOT connect to GPIO1 or GPIO2 (those are buttons!)

4. **Plug in both USB cables:**
   - ESP-Prog → Mac USB port
   - ESP32-S2 → Mac USB-C port (for power and flashing)

### Why Not GPIO1/GPIO2?

On the **Reverse TFT board**:
- **GPIO1** = Button A (user button)
- **GPIO2** = Button B (user button)

**These are NOT UART pins!** If you connect to them, you'll short out the buttons or get garbage data.

---

## Driver Installation (macOS)

### Step 1: Check if Drivers Already Work

```bash
# Plug in ESP-Prog via USB
# Then check for devices:
ls /dev/cu.usbserial-*
```

**Expected output** (drivers working):
```
/dev/cu.usbserial-1100
/dev/cu.usbserial-1101
```

You should see **TWO** devices:
- Lower number (e.g., 1100) = JTAG channel
- **Higher number (e.g., 1101) = UART channel** ← This is what we want!

### Step 2: Install FTDI Drivers (if needed)

If you DON'T see any `/dev/cu.usbserial-*` devices:

1. **Download FTDI VCP Driver:**
   - Visit: https://ftdichip.com/drivers/vcp-drivers/
   - Select: **macOS**
   - Download latest (e.g., `FTDIUSBSerialDextInstaller_1_5_0.dmg`)

2. **Install:**
   - Open the `.dmg` file
   - Run `FTDIUSBSerialDextInstaller.app`
   - Follow the installer wizard
   - ⚠️ **Important:** Grant permissions in **System Settings → Privacy & Security**

3. **Restart macOS**

4. **Verify installation:**
   ```bash
   ls /dev/cu.usbserial-*
   ```
   You should now see two devices.

### Step 3: Identify the UART Port

**Important:** Use `/dev/cu.*` devices, NOT `/dev/tty.*`!

On macOS:
- `/dev/tty.*` - "call in" devices (can cause "resource busy" errors)
- `/dev/cu.*` - "call out" devices (what we want)

The **two ESP-Prog devices** are typically:
- `/dev/cu.usbserial-XXX0` ← JTAG (lower number)
- `/dev/cu.usbserial-XXX1` ← **UART** (higher number) ← Use this one!

**To verify which is UART:**
```bash
# Try the higher numbered port
screen /dev/cu.usbserial-1101 115200

# Press RESET on ESP32-S2
# If you see boot messages → This is the UART port! ✓

# Exit: Ctrl+A, then K, then Y
```

---

## Using UART Console

### Quick Start

```bash
# 1. Connect to console (replace with your actual port)
screen /dev/cu.usbserial-1101 115200

# Screen will be blank - that's normal!
# Logs appear when ESP32-S2 boots or resets
```

### Step-by-Step First Use

#### Step 1: Open Console in Terminal 1

```bash
# Use the higher-numbered cu.* device
screen /dev/cu.usbserial-1101 115200
```

Screen will show a blank terminal - **this is expected**. Logs only appear when the device boots.

#### Step 2: Build and Flash in Terminal 2

Open a **NEW terminal** (keep `screen` running in the first):

```bash
# Navigate to workspace
cd /Users/lukaszronka/projects/zephyr-4.2

# Setup environment
source ./.venv/bin/activate
source ~/zephyr_sdk_0.17.0.sh
source zephyr/zephyr-env.sh

# Build for Reverse board (note the "_reverse" suffix!)
west build -p always -b adafruit_feather_esp32s2_tft_reverse WeatherSensor

# Flash to device
west flash --esp-device /dev/cu.usbmodem01
```

#### Step 3: Watch the Logs!

Go back to **Terminal 1** (the `screen` session).

Press **RESET** button on ESP32-S2.

You should see:

```
ESP-ROM:esp32s2-rc4-20191025
Build:Oct 25 2019
rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)
...
*** Booting Zephyr OS build v4.2.0 ***

=====================================
WeatherSensor - Display Test
ESP32-S2 Reverse TFT Feather
=====================================
UART Console Active (GPIO43)

[00:00:00.123,000] <inf> display_test: === Hardware Initialization ===
[00:00:00.234,000] <inf> display_test: LED configured on GPIO13
[00:00:00.456,000] <inf> display_test: Display power enabled (GPIO7)
[00:00:00.567,000] <inf> display_test: Display ready: st7789v@0
[00:00:00.678,000] <inf> display_test: Display: 135x240, format=1
[00:00:00.789,000] <inf> display_test: Backlight enabled (GPIO45)
[00:00:00.890,000] <inf> display_test: === Hardware initialization complete ===
...
```

🎉 **Success!** You're seeing console logs!

#### Step 4: Exit Screen

To disconnect:
1. Press **Ctrl+A**
2. Press **K**
3. Press **Y** to confirm

#### Step 5: Reconnect Anytime

```bash
screen /dev/cu.usbserial-1101 115200
```

---

## Hardware Setup - JTAG (Advanced)

### ⚠️ Warning: JTAG is Optional

**For 90% of development, UART console logging is enough!**

JTAG is only needed for:
- Setting breakpoints in code
- Stepping through code line-by-line
- Inspecting variables while program is paused
- Debugging hard crashes

### JTAG Pin Accessibility Issue

On the Feather ESP32-S2, JTAG pins (GPIO39-42) are:
- **Not broken out** to the main header pins
- May be accessible on **test pads** on the back of the PCB
- May require **soldering wires** to use

**For now, skip JTAG and just use UART logging!**

### JTAG Port Pinout (for reference)

If you do want to use JTAG later:

```
JTAG Connector (2x3 pins):
┌──────┬──────┐
│ VDD  │ GND  │  Row 1
│ TMS  │ TCK  │  Row 2
│ TDO  │ TDI  │  Row 3
└──────┴──────┘
```

### JTAG Wiring (if accessible)

```
ESP-Prog JTAG Port            ESP32-S2 Reverse TFT
┌─────────────────┐           ┌──────────────────────┐
│ TMS ────────────┼──────────►│ GPIO42 (MTMS)        │
│ TCK ────────────┼──────────►│ GPIO39 (MTCK)        │
│ TDO ────────────┼──────────►│ GPIO40 (MTDO)        │
│ TDI ────────────┼──────────►│ GPIO41 (MTDI)        │
│ GND ────────────┼──────────►│ GND                  │
│ VDD             │  (NC)     │                      │
└─────────────────┘           └──────────────────────┘
```

**Pin mapping:**
- **TMS** → GPIO42
- **TCK** → GPIO39
- **TDO** → GPIO40
- **TDI** → GPIO41
- **GND** → GND (required!)
- **VDD** → NOT CONNECTED

---

## Using JTAG Debugging

### Prerequisites

1. **JTAG wires connected** (see Hardware Setup - JTAG section)
2. **OpenOCD installed**:
   ```bash
   which openocd
   # If not found:
   brew install openocd
   ```

### Basic Debugging Workflow

#### Step 1: Build with Debug Symbols

```bash
cd /Users/lukaszronka/projects/zephyr-4.2
source ./.venv/bin/activate
source ~/zephyr_sdk_0.17.0.sh
source zephyr/zephyr-env.sh

# Build with debug info for Reverse board
west build -b adafruit_feather_esp32s2_tft_reverse WeatherSensor -- -DCMAKE_BUILD_TYPE=Debug
```

#### Step 2: Flash Firmware

```bash
west flash --esp-device /dev/cu.usbmodem01
```

#### Step 3: Start Debug Session

```bash
# This starts OpenOCD and GDB
west debug
```

You'll see the GDB prompt:
```
(gdb) _
```

#### Step 4: Common GDB Commands

```gdb
# Set breakpoint at main function
(gdb) break main

# Run program (will stop at main)
(gdb) continue

# Step to next line
(gdb) next

# Step into function
(gdb) step

# Print variable value
(gdb) print led_state

# Show source code around current location
(gdb) list

# Show call stack
(gdb) backtrace

# Continue execution
(gdb) continue

# Quit debugger
(gdb) quit
```

### Using UART + JTAG Simultaneously

You can use **both at the same time** for maximum debugging power!

**Terminal 1** - UART console logs:
```bash
screen /dev/cu.usbserial-1101 115200
```

**Terminal 2** - JTAG debugging with breakpoints:
```bash
cd /Users/lukaszronka/projects/zephyr-4.2
west debug
```

**Terminal 3** - Optional, for building:
```bash
cd /Users/lukaszronka/projects/zephyr-4.2
# Setup environment...
west build ...
```

This lets you:
- See console logs in real-time (Terminal 1)
- Set breakpoints and inspect variables (Terminal 2)
- Rebuild code (Terminal 3)

---

## Troubleshooting

### Problem: No `/dev/cu.usbserial-*` devices appear

**Possible causes:**
- FTDI drivers not installed
- USB cable is charge-only (need data cable)
- USB port issue on Mac
- ESP-Prog not powered

**Solutions:**
1. Install FTDI VCP drivers (see Driver Installation section)
2. Try a different USB cable (must be data cable, not charge-only)
3. Try different USB port on Mac
4. Check ESP-Prog has power LED lit
5. Unplug and replug ESP-Prog
6. Restart Mac

### Problem: Using `/dev/tty.*` gives "resource busy" error

**Solution:**
Use `/dev/cu.*` devices instead:
```bash
# WRONG (causes "resource busy"):
screen /dev/tty.usbserial-1101 115200

# CORRECT:
screen /dev/cu.usbserial-1101 115200
```

On macOS, always use `cu.*` for serial communication.

### Problem: `screen` shows nothing

**Possible causes:**
- Connected to JTAG port instead of UART port
- ESP32-S2 not booting
- Wrong board target (built for wrong variant)
- Wiring incorrect

**Solutions:**
1. Try the **other** `/dev/cu.usbserial-*` device (higher number = UART)
2. Press **RESET** button on ESP32-S2
3. Verify you built for `adafruit_feather_esp32s2_tft_reverse` (not `..._tft`)
4. Check wiring:
   - GND to GND ✓
   - TXD0 to GPIO43 (DB pin) ✓
   - RXD0 to GPIO44 (optional) ✓
   - **NOT connected to GPIO1/GPIO2!** ✗
5. Try flashing firmware again
6. Look for any output at all (even garbled text means you're close!)

### Problem: Garbled text / random hex characters

**Example:**
```
00 ab b5 a5 b6 01 00 f6 a5 b5 ...
```

**Possible causes:**
- Wrong baud rate
- Wrong board target (mixing Regular and Reverse)
- Poor wire connection

**Solutions:**
1. Verify baud rate is **115200**:
   ```bash
   screen /dev/cu.usbserial-1101 115200  # ← Must be 115200
   ```

2. **Rebuild for correct board:**
   ```bash
   # Make sure you're using "_reverse" suffix!
   west build -p always -b adafruit_feather_esp32s2_tft_reverse WeatherSensor
   west flash --esp-device /dev/cu.usbmodem01
   ```

3. Check wiring is solid (wiggle test)

4. Try different jumper wires

### Problem: "I connected to GPIO1/GPIO2 and nothing works!"

**Solution:**
**GPIO1 and GPIO2 are BUTTONS on the Reverse board!** You must use GPIO43/GPIO44 instead.

Correct connections:
- ESP-Prog **Pin 3 (ESP_TXD)** → ESP32-S2 **GPIO43 (DB pin)**
- ESP-Prog **Pin 5 (ESP_RXD)** → ESP32-S2 **GPIO44**
- ESP-Prog **Pin 4 (GND)** → ESP32-S2 GND

### Problem: `west flash` fails

**Possible causes:**
- Not in bootloader mode
- Wrong USB port specified
- USB-C cable issue

**Solutions:**
1. Properly enter bootloader mode:
   - Hold **BOOT** button
   - Press and release **RESET** button
   - Release **BOOT** after 1 second

2. Try with explicit port:
   ```bash
   west flash --esp-device /dev/cu.usbmodem01
   ```

3. Check USB-C cable is connected to ESP32-S2

4. Try auto-detect (omit `--esp-device`):
   ```bash
   west flash
   ```

### Problem: Built for wrong board variant

**Symptoms:**
- ROM bootloader messages visible, then garbage when Zephyr starts
- Display doesn't work
- UART console garbled

**Solution:**
Always use the **Reverse** board target (this is the ONLY board supported in this project):
```bash
# CORRECT - the only valid board target:
west build -b adafruit_feather_esp32s2_tft_reverse WeatherSensor

# WRONG - this is a different board (regular TFT, not Reverse):
# west build -b adafruit_feather_esp32s2_tft WeatherSensor
```

### Problem: Can't exit `screen`

**Solution:**
1. Press **Ctrl+A** (release)
2. Press **K** (release)
3. Press **Y** to confirm

If that doesn't work:
```bash
# In a new terminal, find all screen sessions
screen -ls

# Kill specific session
screen -X -S <session-id> quit

# Or kill all screen sessions
killall screen
```

---

## Quick Reference

### Reverse Board Pin Summary

```
⚠️ CRITICAL: Reverse Board Pin Assignments ⚠️
──────────────────────────────────────────────
Console UART:  GPIO43 (DB pin, TX) + GPIO44 (RX)
GPIO1:         BUTTON A (NOT UART!)
GPIO2:         BUTTON B (NOT UART!)
Board Target:  adafruit_feather_esp32s2_tft_reverse
```

### Connection Summary

```
ESP-Prog PROGRAM Port Connections:
──────────────────────────────────────────────────────────
Pin 4 (GND)      →  ESP32-S2 GND
Pin 3 (ESP_TXD)  →  ESP32-S2 GPIO43 (DB pin, TX)  ⚠️ NOT CROSSED!
Pin 5 (ESP_RXD)  →  ESP32-S2 GPIO44 (RX)          ⚠️ Optional
Pin 2 (VDD)      →  NOT CONNECTED
Pin 1 (ESP_EN)   →  NOT CONNECTED
Pin 6 (ESP_IO0)  →  NOT CONNECTED

IMPORTANT: ESP-Prog has internal TX/RX crossover!
           ESP_TXD (pin 3) connects to TX (GPIO43)
           ESP_RXD (pin 5) connects to RX (GPIO44)
           (NOT crossed like normal UART adapters!)
```

### Console Commands

```bash
# Find serial ports (use cu.* not tty.*)
ls /dev/cu.usbserial-*

# Connect to UART console (use higher numbered port)
screen /dev/cu.usbserial-1101 115200

# Exit screen: Ctrl+A, then K, then Y
```

### Build & Flash Commands

```bash
# Setup environment
cd /Users/lukaszronka/projects/zephyr-4.2
source ./.venv/bin/activate
source ~/zephyr_sdk_0.17.0.sh
source zephyr/zephyr-env.sh

# Build for Reverse board (note the "_reverse" suffix!)
west build -b adafruit_feather_esp32s2_tft_reverse WeatherSensor

# Clean build
west build -p always -b adafruit_feather_esp32s2_tft_reverse WeatherSensor

# Flash (bootloader mode: hold BOOT, tap RESET, release BOOT)
west flash --esp-device /dev/cu.usbmodem01
```

### Environment Setup Alias (Optional)

Add to `~/.zshrc` or `~/.bashrc`:

```bash
alias zephyr-setup='cd /Users/lukaszronka/projects/zephyr-4.2 && source ./.venv/bin/activate && source ~/zephyr_sdk_0.17.0.sh && source zephyr/zephyr-env.sh'
```

Then just:
```bash
zephyr-setup
west build -b adafruit_feather_esp32s2_tft_reverse WeatherSensor
```

### Debug Commands (JTAG)

```bash
# Build with debug info
west build -b adafruit_feather_esp32s2_tft_reverse WeatherSensor -- -DCMAKE_BUILD_TYPE=Debug

# Flash
west flash --esp-device /dev/cu.usbmodem01

# Start debugging session
west debug

# Or attach to running program
west attach
```

### Common GDB Commands

```gdb
break main                # Set breakpoint at main()
break main_simple.cpp:100 # Set breakpoint at line 100
continue                  # Continue execution
next                      # Step over (next line)
step                      # Step into function
print variable_name       # Print variable value
list                      # Show source code
backtrace                 # Show call stack
info locals               # Show local variables
info breakpoints          # List all breakpoints
delete 1                  # Delete breakpoint #1
quit                      # Exit GDB
```

---

## Pin Reference Tables

### ESP32-S2 Reverse TFT Console Pins

| Function | GPIO | Feather Label | ESP-Prog Connection | Notes |
|----------|------|---------------|---------------------|-------|
| Console TX | **GPIO43** | **DB** | ESP-Prog **Pin 3 (ESP_TXD)** | ⚠️ NOT GPIO1! |
| Console RX | **GPIO44** | (internal) | ESP-Prog **Pin 5 (ESP_RXD)** | Optional |
| Ground | - | GND | ESP-Prog **Pin 4 (GND)** | Required |
| Button A | GPIO1 | GPIO1 | - | **NOT UART!** |
| Button B | GPIO2 | GPIO2 | - | **NOT UART!** |

### ESP32-S2 JTAG Pins (Advanced)

| Function | GPIO | ESP-Prog JTAG Pin | Accessibility |
|----------|------|-------------------|---------------|
| MTMS | GPIO42 | TMS | Test pad / not broken out |
| MTCK | GPIO39 | TCK | Test pad / not broken out |
| MTDO | GPIO40 | TDO | Test pad / not broken out |
| MTDI | GPIO41 | TDI | Test pad / not broken out |
| Ground | - | GND | Main headers |

---

## Resources

- **ESP-Prog Official Guide**: https://docs.espressif.com/projects/espressif-esp-iot-solution/en/latest/hw-reference/ESP-Prog_guide.html
- **Adafruit Reverse TFT Pinout**: https://learn.adafruit.com/esp32-s2-reverse-tft-feather/pinouts
- **ESP32-S2 Datasheet**: https://www.espressif.com/sites/default/files/documentation/esp32-s2_datasheet_en.pdf
- **Zephyr Debugging Guide**: https://docs.zephyrproject.org/latest/develop/debug/index.html
- **OpenOCD Documentation**: https://openocd.org/doc/html/index.html
- **GDB Quick Reference**: https://darkdust.net/files/GDB%20Cheat%20Sheet.pdf

---

## Summary Checklist

### Initial Setup (Do Once)

- [ ] Install FTDI drivers on macOS
- [ ] Identify DB pin (GPIO43) on Reverse TFT board
- [ ] Connect ESP-Prog UART wires:
  - [ ] **Pin 4 (GND)** → GND
  - [ ] **Pin 3 (ESP_TXD)** → GPIO43 (DB pin) ⚠️ NOT GPIO1!
  - [ ] **Pin 5 (ESP_RXD)** → GPIO44 (optional)
- [ ] Plug in ESP-Prog USB
- [ ] Verify two `/dev/cu.usbserial-*` devices appear
- [ ] Test console: `screen /dev/cu.usbserial-1101 115200`
- [ ] Flash test firmware and see logs
- [ ] Verify correct board target: `adafruit_feather_esp32s2_tft_reverse`

### Daily Development Workflow

- [ ] Open console: `screen /dev/cu.usbserial-1101 115200`
- [ ] Setup environment in another terminal
- [ ] Make code changes
- [ ] Build: `west build -b adafruit_feather_esp32s2_tft_reverse WeatherSensor`
- [ ] Flash: `west flash --esp-device /dev/cu.usbmodem01`
- [ ] Press RESET, watch logs in screen terminal
- [ ] Debug with console logs
- [ ] Iterate!

### When You Need JTAG (Rare)

- [ ] Verify JTAG pins are accessible (may need soldering)
- [ ] Connect JTAG wires (GPIO39-42)
- [ ] Build with debug: `-- -DCMAKE_BUILD_TYPE=Debug`
- [ ] Flash firmware
- [ ] Start debug: `west debug`
- [ ] Set breakpoints, step through code
- [ ] Inspect variables, find bugs
- [ ] Fix and rebuild

---

## Key Takeaways

1. **Reverse board uses GPIO43/44** (DB pin), NOT GPIO1/GPIO2
2. **GPIO1/GPIO2 are buttons** on Reverse board
3. **ESP-Prog has internal crossover** - connect TXD0→TX, RXD0→RX (not crossed)
4. **Always use `cu.*` devices** on macOS, not `tty.*`
5. **Board target must include "_reverse"** suffix
6. **UART console is sufficient** for 90% of development

**You're all set!** The DB pin is your window into the ESP32-S2. Start coding! 🚀
