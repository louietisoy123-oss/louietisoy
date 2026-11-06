/**
 * @file buttons.c
 * @brief Debounced KEY0/KEY1 (PC0/PC1) handling: acknowledge-alert and
 *        test-alert toggle for the Smart Baby Monitoring System.
 *
 * Pin setup and active-LOW read modeled on Laboratory 4 service_mode.c
 * (raw GPIOC registers, pull-ups enabled). Debounce is time-stamped rather
 * than poll-counted because this module is driven from the UI thread's
 * 500 ms cycle: a raw level held stable for DEBOUNCE_MS becomes the new
 * stable level, and a press is the stable level's falling edge.
 */

#include "buttons.h"
#include "net_task.h"
#include "baby_logic.h"
#include "monitor_state.h"
#include "ui_dashboard.h"
#include "wifi_storage.h"

#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <stm32f4xx.h>

/* KEY0 = PC0, KEY1 = PC1 on the RT-Spark board, both active-LOW. */
#define KEY0_BIT 0U
#define KEY1_BIT 1U

/* A raw level must hold this long (ms) before it is accepted as stable. */
#define DEBOUNCE_MS 50U
#define PORTAL_HOLD_MS 3000U /* KEY0 held 3 s opens the config portal */

/* Per-key debounce bookkeeping. stable starts HIGH (= released). */
typedef struct {
	bool stable;         /* accepted (debounced) level, true = released */
	bool last_raw;       /* previous raw sample                         */
	uint32_t raw_change_ms; /* k_uptime_get_32() of last raw transition  */
	uint32_t held_since_ms; /* 0 = not tracking a hold                   */
	bool portal_fired;   /* KEY0 long-hold fired for this press          */
} key_t;

static key_t s_key0 = { true, true, 0U };
static key_t s_key1 = { true, true, 0U };

void buttons_init(void)
{
	/* PC0 (KEY0) and PC1 (KEY1): inputs with pull-up, active LOW
	 * (Laboratory 4 service_mode.c register recipe). */
	RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
	(void)RCC->AHB1ENR; /* settle after clock enable */
	GPIOC->MODER &= ~((3U << (KEY0_BIT * 2)) | (3U << (KEY1_BIT * 2)));
	GPIOC->PUPDR = (GPIOC->PUPDR &
			~((3U << (KEY0_BIT * 2)) | (3U << (KEY1_BIT * 2)))) |
		       ((1U << (KEY0_BIT * 2)) | (1U << (KEY1_BIT * 2)));

	s_key0.stable = true;
	s_key0.last_raw = true;
	s_key0.raw_change_ms = 0U;
	s_key0.held_since_ms = 0U;
	s_key0.portal_fired = false;
	s_key1.stable = true;
	s_key1.last_raw = true;
	s_key1.raw_change_ms = 0U;
}

/* Debounce one key. Returns true exactly once per confirmed press
 * (falling edge of the debounced level). */
static bool key_update(key_t *k, bool raw_now, uint32_t now_ms)
{
	if (raw_now != k->last_raw) {
		/* Raw bounce: (re)start the stability window. */
		k->last_raw = raw_now;
		k->raw_change_ms = now_ms;
	}
	if ((raw_now != k->stable) &&
	    ((now_ms - k->raw_change_ms) >= DEBOUNCE_MS)) {
		k->stable = raw_now;
		if (!k->stable) {
			return true; /* confirmed press (falling edge) */
		}
		/* Released: stop any long-hold tracking. */
		k->held_since_ms = 0U;
		k->portal_fired = false;
	}
	/* KEY0 long-hold (>= PORTAL_HOLD_MS while pressed): request the
	 * config portal once per press (on-demand reconfiguration even
	 * when Wi-Fi is working; the ack already fired on the press). */
	if (!k->stable && k == &s_key0) {
		if (k->held_since_ms == 0U) {
			k->held_since_ms = now_ms;
		} else if (!k->portal_fired &&
			   (now_ms - k->held_since_ms) >= PORTAL_HOLD_MS) {
			k->portal_fired = true;
			net_task_request_portal();
			printk("[UI] KEY0 held -- config portal requested\n");
		}
	}
	return false;
}

static inline bool key0_raw_pressed(void)
{
	return (GPIOC->IDR & (1U << KEY0_BIT)) == 0U;
}

static inline bool key1_raw_pressed(void)
{
	return (GPIOC->IDR & (1U << KEY1_BIT)) == 0U;
}

/* Lab 3 interactive Wi-Fi reconfiguration dialog: triggered when KEY0 is
 * pressed while no unmuted alarm is sounding. Shows an 8 s confirmation
 * screen on the LCD ([KEY0] CONFIRM & RESET / [KEY1] CANCEL & RESUME). */
static void run_wifi_reconfig_dialog(void)
{
	printk("[KEY0] Wi-Fi reconfigure requested. Showing confirmation on LCD...\n");
	while (key0_raw_pressed()) {
		k_msleep(10);
	}

	int countdown = 8;
	bool confirmed = false;
	uint32_t t_last = k_uptime_get_32();

	while (countdown > 0) {
		ui_dashboard_show_reset_confirm(net_task_get_ssid(), countdown);

		if (key0_raw_pressed()) {
			k_msleep(50);
			if (key0_raw_pressed()) {
				confirmed = true;
				while (key0_raw_pressed()) {
					k_msleep(10);
				}
				break;
			}
		}

		if (key1_raw_pressed()) {
			k_msleep(50);
			if (key1_raw_pressed()) {
				printk("[KEY1] Reconfiguration cancelled by user.\n");
				while (key1_raw_pressed()) {
					k_msleep(10);
				}
				break;
			}
		}

		k_msleep(50);
		if ((k_uptime_get_32() - t_last) >= 1000U) {
			countdown--;
			t_last = k_uptime_get_32();
		}
	}

	ui_dashboard_show_reset_confirm(NULL, 0);

	if (confirmed) {
		printk("[WIFI] Reset confirmed by user! Launching Setup Portal...\n");
		wifi_storage_clear();
		net_task_request_portal();
	}

	/* Repaint the main Baby Monitor dashboard frame and reset key state */
	ui_dashboard_init();
	s_key0.stable = true;
	s_key0.last_raw = true;
	s_key0.held_since_ms = 0U;
	s_key0.portal_fired = false;
	s_key1.stable = true;
	s_key1.last_raw = true;
}

static void on_key0_press(void)
{
	/* Caregiver acknowledge: refresh the logic mute window and mirror it
	 * into the shared snapshot for the dashboard MUTE row. If the buzzer
	 * was not actively sounding, open the Lab 3 Wi-Fi reconfigure dialog
	 * so a single KEY0 press lets the user start the BabyMon-Setup hotspot. */
	uint32_t now = k_uptime_get_32();
	bool was_ringing;

	monitor_state_lock();
	was_ringing = monitor_state_get()->buzzer_on ||
		      monitor_state_get()->test_mode;
	baby_logic_acknowledge_for(now, ALERT_MUTE_MS);
	monitor_state_get()->mute_until_ms = now + ALERT_MUTE_MS;
	monitor_state_get()->buzzer_on =
		baby_logic_buzzer_on(monitor_state_get()->state, now);
	monitor_state_unlock();

	printk("[UI] Alert acknowledged (mute %u s)\n",
	       (unsigned)(ALERT_MUTE_MS / 1000U));

	if (!was_ringing) {
		run_wifi_reconfig_dialog();
	}
}

static void on_key1_press(void)
{
	/* test_mode is a UI-level demo alert: it only exercises the alarm
	 * outputs (buzzer + LED tint) so the presentation layer can be
	 * demonstrated without a fault. It never claims real cry detection
	 * and is deliberately invisible to baby_logic / the sensor task. */
	bool on;

	monitor_state_lock();
	monitor_state_get()->test_mode = !monitor_state_get()->test_mode;
	on = monitor_state_get()->test_mode;
	monitor_state_unlock();

	printk("[UI] Test alert %s\n", on ? "ON" : "OFF");
}

void buttons_poll(uint32_t now_ms)
{
	/* true from IDR = level high = released (active-LOW keys). */
	const bool raw0 = (GPIOC->IDR & (1U << KEY0_BIT)) != 0U;
	const bool raw1 = (GPIOC->IDR & (1U << KEY1_BIT)) != 0U;

	if (key_update(&s_key0, raw0, now_ms)) {
		on_key0_press();
	}
	if (key_update(&s_key1, raw1, now_ms)) {
		on_key1_press();
	}
}
