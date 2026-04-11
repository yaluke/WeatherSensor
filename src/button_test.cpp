/*
 * Minimal button test for ESP32-S2 Reverse TFT
 * Tests D0 (GPIO0), D1 (GPIO1), D2 (GPIO2) via:
 *   1. Zephyr input subsystem (interrupt-based)
 *   2. Direct GPIO polling (fallback check)
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(button_test, LOG_LEVEL_INF);

/* Direct GPIO specs for polling test */
static const struct gpio_dt_spec btn_d0 =
	GPIO_DT_SPEC_GET(DT_NODELABEL(user_button), gpios);
static const struct gpio_dt_spec btn_d1 =
	GPIO_DT_SPEC_GET(DT_NODELABEL(d1_button), gpios);
static const struct gpio_dt_spec btn_d2 =
	GPIO_DT_SPEC_GET(DT_NODELABEL(d2_button), gpios);

/* Input subsystem callback — logs ALL events */
static void input_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	LOG_INF("[INPUT] type=%u code=%u value=%d sync=%u",
		evt->type, evt->code, evt->value, evt->sync);
}
INPUT_CALLBACK_DEFINE(NULL, input_cb, NULL);

int main(void)
{
	LOG_INF("=== BUTTON TEST START ===");
	LOG_INF("D0=GPIO%d D1=GPIO%d D2=GPIO%d",
		btn_d0.pin, btn_d1.pin, btn_d2.pin);

	/* Check GPIO readiness */
	LOG_INF("D0 ready: %d", gpio_is_ready_dt(&btn_d0));
	LOG_INF("D1 ready: %d", gpio_is_ready_dt(&btn_d1));
	LOG_INF("D2 ready: %d", gpio_is_ready_dt(&btn_d2));

	/* Configure as inputs (gpio-keys driver already did this,
	 * but let's verify we can read them) */
	int prev_d0 = 0, prev_d1 = 0, prev_d2 = 0;

	LOG_INF("Polling buttons + listening for input events...");
	LOG_INF("Press each button (D0, D1, D2) and check logs.");
	LOG_INF("");

	while (1) {
		int d0 = gpio_pin_get_dt(&btn_d0);
		int d1 = gpio_pin_get_dt(&btn_d1);
		int d2 = gpio_pin_get_dt(&btn_d2);

		/* Log state changes from polling */
		if (d0 != prev_d0) {
			LOG_INF("[POLL] D0 (GPIO%d): %d -> %d",
				btn_d0.pin, prev_d0, d0);
			prev_d0 = d0;
		}
		if (d1 != prev_d1) {
			LOG_INF("[POLL] D1 (GPIO%d): %d -> %d",
				btn_d1.pin, prev_d1, d1);
			prev_d1 = d1;
		}
		if (d2 != prev_d2) {
			LOG_INF("[POLL] D2 (GPIO%d): %d -> %d",
				btn_d2.pin, prev_d2, d2);
			prev_d2 = d2;
		}

		k_msleep(50); /* Poll at 20Hz */
	}

	return 0;
}
