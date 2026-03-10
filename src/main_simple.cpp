/*
 * Copyright (c) 2026 Lukasz Ronka
 * SPDX-License-Identifier: Apache-2.0
 *
 * WeatherSensor - Display Test Application
 *
 * This is a minimal test application for the ESP32-S2 Reverse TFT Feather.
 * It demonstrates:
 * - GPIO control (LED, display power, backlight)
 * - Display buffer management and rendering
 * - Basic Zephyr device model usage
 * - Memory-constrained drawing techniques
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(display_test, LOG_LEVEL_INF);

/*
 * Hardware GPIO Definitions
 *
 * These are resolved from the device tree at compile time.
 * See boards/adafruit_feather_esp32s2_tft_reverse.overlay for configuration.
 */

/* Status LED (GPIO13 on Reverse board) - used for visual feedback */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* Backlight control (GPIO45 on Reverse board) - enables TFT backlight */
static const struct gpio_dt_spec backlight = GPIO_DT_SPEC_GET(DT_ALIAS(backlight), gpios);

/*
 * Display power control (GPIO7 on Reverse board)
 * CRITICAL: This GPIO must be HIGH before initializing the display!
 * Without this, the TFT will not respond to any commands.
 *
 * NOTE: We tried using Zephyr's power domain subsystem (CONFIG_PM), but it
 * causes boot crashes on this board. Manual GPIO control is the reliable
 * approach for now.
 */
static const struct gpio_dt_spec display_power =
	GPIO_DT_SPEC_GET_BY_IDX(DT_PATH(i2c_reg), enable_gpios, 0);

/* Display device - ST7789V controller on SPI2 */
static const struct device *display_dev;

/*
 * RGB565 Color Definitions
 *
 * Format: 16-bit color (5 bits red, 6 bits green, 5 bits blue)
 * Used directly by the ST7789V display controller.
 */
#define RGB565_RED     0xF800
#define RGB565_GREEN   0x07E0
#define RGB565_BLUE    0x001F
#define RGB565_WHITE   0xFFFF
#define RGB565_BLACK   0x0000
#define RGB565_YELLOW  0xFFE0
#define RGB565_CYAN    0x07FF
#define RGB565_MAGENTA 0xF81F

/* Drawing constants */
#define STRIP_HEIGHT 30  /* Height of drawing strips (memory optimization) */

/**
 * @brief Fill a rectangular area with a solid color
 *
 * This function allocates a temporary buffer, fills it with the specified
 * color, and writes it to the display via the display driver.
 *
 * MEMORY NOTE: The buffer is allocated from the heap (CONFIG_HEAP_MEM_POOL_SIZE).
 * For this ESP32-S2 with 320KB RAM, we allocate in strips to avoid large
 * contiguous allocations. Full screen (135x240 = 64,800 bytes) would fail.
 *
 * @param x      X coordinate (top-left)
 * @param y      Y coordinate (top-left)
 * @param w      Width in pixels
 * @param h      Height in pixels
 * @param color  RGB565 color value
 * @return 0 on success, negative errno on failure
 */
static int fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
	struct display_buffer_descriptor buf_desc;
	uint16_t *buf;
	size_t buf_size = w * h * sizeof(uint16_t);
	int ret;

	/* Allocate display buffer from heap */
	buf = (uint16_t *)k_malloc(buf_size);
	if (!buf) {
		LOG_ERR("Failed to allocate %d bytes for %dx%d rectangle",
			buf_size, w, h);
		return -ENOMEM;
	}

	/* Fill entire buffer with the same color value */
	for (size_t i = 0; i < w * h; i++) {
		buf[i] = color;
	}

	/* Configure buffer descriptor for display driver */
	buf_desc.buf_size = buf_size;
	buf_desc.width = w;
	buf_desc.height = h;
	buf_desc.pitch = w;  /* Row stride (pixels per row) */

	/* Write buffer to display via SPI */
	ret = display_write(display_dev, x, y, &buf_desc, buf);
	if (ret < 0) {
		LOG_ERR("display_write failed at (%d,%d): %d", x, y, ret);
	}

	k_free(buf);
	return ret;
}

/*
 * ============================================================================
 * INITIALIZATION FUNCTIONS
 * ============================================================================
 */

/**
 * @brief Initialize and test the status LED
 *
 * Configures the LED as output and performs 3 quick blinks to indicate
 * successful boot. This provides immediate visual feedback before UART
 * console is available.
 *
 * @return 0 on success, negative errno on failure
 */
static int init_led(void)
{
	int ret;

	if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("LED GPIO not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure LED: %d", ret);
		return ret;
	}

	LOG_INF("LED configured on GPIO%d", led.pin);

	/* Three quick blinks to indicate boot success */
	for (int i = 0; i < 3; i++) {
		gpio_pin_set_dt(&led, 1);
		k_msleep(200);
		gpio_pin_set_dt(&led, 0);
		k_msleep(200);
	}

	return 0;
}

/**
 * @brief Enable display power supply
 *
 * CRITICAL: The display power (GPIO7) MUST be enabled before accessing
 * the display controller. On the Reverse TFT board, this GPIO controls
 * a power switch to the ST7789V display.
 *
 * @return 0 on success, negative errno on failure
 */
static int init_display_power(void)
{
	int ret;

	if (!gpio_is_ready_dt(&display_power)) {
		LOG_ERR("Display power GPIO not ready - display will not work!");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&display_power, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure display power: %d", ret);
		return ret;
	}

	/* Turn on display power */
	gpio_pin_set_dt(&display_power, 1);
	LOG_INF("Display power enabled (GPIO%d)", display_power.pin);

	/* Wait for power rail to stabilize before display init */
	k_msleep(10);

	return 0;
}

/**
 * @brief Enable display backlight
 *
 * Controls the TFT backlight via GPIO45. The backlight can be turned on/off
 * independently of the display controller to save power.
 *
 * @return 0 on success, negative errno on failure
 */
static int init_backlight(void)
{
	int ret;

	if (!gpio_is_ready_dt(&backlight)) {
		LOG_WRN("Backlight GPIO not available");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&backlight, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure backlight: %d", ret);
		return ret;
	}

	/* Turn on backlight */
	gpio_pin_set_dt(&backlight, 1);
	LOG_INF("Backlight enabled (GPIO%d)", backlight.pin);

	return 0;
}

/**
 * @brief Initialize the display controller
 *
 * Gets the display device from device tree and verifies it's ready.
 * Also retrieves and logs display capabilities.
 *
 * @param caps  Pointer to structure to receive display capabilities
 * @return 0 on success, negative errno on failure
 */
static int init_display(struct display_capabilities *caps)
{
	int ret;

	/* Get display device from device tree */
	display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!display_dev) {
		LOG_ERR("Display device not found in device tree");
		return -ENODEV;
	}

	if (!device_is_ready(display_dev)) {
		LOG_ERR("Display device not ready");
		return -ENODEV;
	}

	LOG_INF("Display ready: %s", display_dev->name);

	/* Query display capabilities */
	display_get_capabilities(display_dev, caps);
	LOG_INF("Display: %dx%d, format=%d",
		caps->x_resolution, caps->y_resolution,
		caps->current_pixel_format);

	/* Enable display output (turn off blanking) */
	ret = display_blanking_off(display_dev);
	if (ret < 0) {
		LOG_WRN("Failed to disable blanking: %d", ret);
	}

	/* Brief delay for display to stabilize */
	k_msleep(100);

	return 0;
}

/*
 * ============================================================================
 * MAIN APPLICATION
 * ============================================================================
 */

/**
 * @brief Main application entry point
 *
 * Initializes hardware and runs a simple display test that:
 * 1. Shows 8 colored bars across the screen
 * 2. Cycles through solid colors every 2 seconds
 * 3. Blinks the LED at 1Hz as a heartbeat
 */
int main(void)
{
	int ret;
	bool led_state = false;
	struct display_capabilities caps;
	uint16_t test_colors[] = {
		RGB565_RED, RGB565_GREEN, RGB565_BLUE,
		RGB565_YELLOW, RGB565_CYAN, RGB565_MAGENTA,
		RGB565_WHITE, RGB565_BLACK
	};
	int color_idx = 0;
	uint16_t bar_height;  /* Declared early to avoid jump-over-initialization error */

	/* Print boot banner (uses printk for immediate output) */
	printk("\n\n\n");
	printk("=====================================\n");
	printk("WeatherSensor - Display Test\n");
	printk("ESP32-S2 Reverse TFT Feather\n");
	printk("=====================================\n");
	printk("UART Console Active (GPIO43)\n");
	printk("\n");

	LOG_INF("=== Hardware Initialization ===");

	/* Initialize LED for visual feedback */
	ret = init_led();
	if (ret < 0) {
		LOG_ERR("LED init failed: %d", ret);
	}

	/*
	 * CRITICAL INITIALIZATION SEQUENCE:
	 * 1. Display power (GPIO7) MUST be enabled before accessing display
	 * 2. Wait 10ms for power rail to stabilize
	 * 3. Display controller init happens via Zephyr driver
	 * 4. Backlight can be enabled anytime after display is ready
	 *
	 * NOTE: We use manual GPIO control because the power management
	 * subsystem (CONFIG_PM) causes boot crashes on this board.
	 */
	ret = init_display_power();
	if (ret < 0) {
		LOG_ERR("Display power init failed: %d", ret);
		goto error;
	}

	ret = init_display(&caps);
	if (ret < 0) {
		LOG_ERR("Display init failed: %d", ret);
		goto error;
	}

	ret = init_backlight();
	if (ret < 0) {
		LOG_WRN("Backlight init failed: %d (continuing anyway)", ret);
	}

	LOG_INF("=== Hardware initialization complete ===");

	/*
	 * DEMO PHASE 1: Draw colored bars
	 * Draws 8 horizontal bars in different colors to verify display function
	 */
	LOG_INF("=== Drawing test pattern ===");

	bar_height = caps.y_resolution / 8;
	for (int i = 0; i < 8; i++) {
		fill_rect(0, i * bar_height, caps.x_resolution,
			  bar_height, test_colors[i]);
		k_msleep(50);  /* Brief delay for visual effect */
	}

	LOG_INF("Test pattern complete");

	/*
	 * DEMO PHASE 2: Color cycling
	 * Fills entire screen with solid colors, cycling every 2 seconds.
	 * LED blinks at 1Hz as a heartbeat indicator.
	 *
	 * NOTE: Drawing is done in strips to avoid large memory allocations.
	 * Full screen = 135x240 = 32,400 pixels = 64,800 bytes (too large!)
	 * Using 30-pixel strips = 135x30 = 4,050 pixels = 8,100 bytes (OK)
	 */
	LOG_INF("Starting color cycle (Ctrl+C to stop)");

	while (1) {
		/* Toggle LED at 1Hz for heartbeat */
		led_state = !led_state;
		gpio_pin_set_dt(&led, led_state);

		/* Change screen color every 2 seconds (on LED toggle) */
		if (led_state) {
			/* Draw screen in horizontal strips */
			for (int y = 0; y < caps.y_resolution; y += STRIP_HEIGHT) {
				uint16_t h = (y + STRIP_HEIGHT > caps.y_resolution) ?
					     (caps.y_resolution - y) : STRIP_HEIGHT;
				fill_rect(0, y, caps.x_resolution, h,
					  test_colors[color_idx]);
			}
			color_idx = (color_idx + 1) % 8;
		}

		k_msleep(1000);
	}

	return 0;

error:
	/* Error handler - fast LED blink indicates failure */
	LOG_ERR("Initialization failed, entering error state");
	while (1) {
		gpio_pin_toggle_dt(&led);
		k_msleep(100);
	}
}
