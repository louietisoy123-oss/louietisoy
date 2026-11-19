/**
 * @file main.c
 * @brief Spark USB DAC: USB Audio Class 2 sound card -> ES8388 -> 3.5 mm jack.
 *
 * Formats offered to the host: 44.1 / 48 / 96 kHz, 24-bit stereo (full-speed USB).
 *
 * Audio path: each 1 ms USB packet is unpacked into a ring buffer (i2s_out.c); a
 * circular DMA streams the ring to I2S3 without ever stopping. The ring fill level
 * steers the explicit-feedback value so the PC sends samples at the codec's real rate.
 *
 * Board UI: the ST7789 LCD shows rate, format, stream state, buffer level, volume and
 * VU meters. D-pad UP/DOWN = codec hardware volume (2 dB steps), CENTER = mute,
 * LEFT/RIGHT = codec stereo enhancement strength (0 = off .. 7).
 * (Zephyr 4.4's UAC2 class does not handle feature-unit requests, so the Windows
 * volume slider scales the audio in software; the buttons drive the codec itself.)
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usbd_msg.h>
#include <zephyr/usb/class/usbd_uac2.h>

#include "buttons.h"
#include "es8388.h"
#include "i2s_out.h"
#include "lcd_st7789.h"
#include "usbd_setup.h"

LOG_MODULE_REGISTER(spark_usb_dac, LOG_LEVEL_INF);

#define DAC_IN_TERMINAL_ID UAC2_ENTITY_ID(DT_NODELABEL(dac_in_terminal))

#define BYTES_PER_SAMPLE    3U                       /* 24-bit subslot */
#define BYTES_PER_FRAME     (2U * BYTES_PER_SAMPLE)
/* Largest packet: 96 samples per frame + 1 (asynchronous endpoint) at 96 kHz */
#define MAX_PACKET_BYTES    ((96U + 1U) * BYTES_PER_FRAME)
#define RX_BUFFERS          4

K_MEM_SLAB_DEFINE_STATIC(rx_slab, ROUND_UP(MAX_PACKET_BYTES, UDC_BUF_GRANULARITY),
			 RX_BUFFERS, UDC_BUF_ALIGN);

static const struct device *const uac2_dev = DEVICE_DT_GET(DT_NODELABEL(uac2_dac));

static volatile uint32_t s_rate_hz = 48000U;
static volatile uint32_t s_pending_rate;        /* set by host, applied by main thread */
static volatile bool s_streaming;

/* Volume: codec DAC attenuation in 0.5 dB steps (0 = full scale) */
static volatile uint8_t s_atten = 24U;          /* start at -12 dB */
static volatile bool s_muted;
static uint8_t s_se;                            /* stereo enhancement 0..7 (0 = off) */

/* Diagnostics */
static volatile uint32_t s_packets;
static volatile uint32_t s_bytes;
static volatile uint32_t s_slab_misses;
static volatile uint32_t s_fb_value;
extern volatile uint32_t g_iso_out_rearms;      /* src/usb_iso_fix.c */
extern volatile uint32_t g_iso_in_rearms;

/* ---------------------------------------------------------------------------------
 * Feedback regulator. Full-speed feedback is Q10.14 samples per 1 ms frame. Target the
 * ring at half full; proportional on the level error, clamped to +/-1 % of nominal,
 * so it can never run away.
 * --------------------------------------------------------------------------------- */
static uint32_t feedback_for_level(uint32_t rate_hz, uint32_t level)
{
	int64_t nominal = ((int64_t)rate_hz << 14) / 1000;
	int64_t target = (int64_t)i2s_out_capacity() / 2;
	int64_t err = (int64_t)level - target;           /* frames above target */
	/* 1 frame of error -> 1/1024 of a sample per frame correction */
	int64_t dev = -(err << 14) / 1024;
	int64_t lim = nominal / 100;

	if (dev > lim) {
		dev = lim;
	} else if (dev < -lim) {
		dev = -lim;
	}
	return (uint32_t)(nominal + dev);
}

static void dac_sof_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	s_fb_value = feedback_for_level(s_rate_hz, i2s_out_level());
}

static void dac_terminal_update_cb(const struct device *dev, uint8_t terminal,
				   bool enabled, bool microframes, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(microframes);
	ARG_UNUSED(user_data);

	if (terminal != DAC_IN_TERMINAL_ID) {
		return;
	}
	s_streaming = enabled;
	if (!enabled) {
		i2s_out_flush();
	}
	printk("[USB] stream %s (%u Hz)\n", enabled ? "OPEN" : "CLOSED", (unsigned)s_rate_hz);
}

static void *dac_get_recv_buf(const struct device *dev, uint8_t terminal,
			      uint16_t size, void *user_data)
{
	void *buf = NULL;

	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);

	if (terminal != DAC_IN_TERMINAL_ID || size > MAX_PACKET_BYTES) {
		return NULL;
	}
	if (k_mem_slab_alloc(&rx_slab, &buf, K_NO_WAIT) != 0) {
		s_slab_misses++;
		return NULL;
	}
	return buf;
}

static void dac_data_recv_cb(const struct device *dev, uint8_t terminal,
			     void *buf, uint16_t size, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(terminal);
	ARG_UNUSED(user_data);

	if (s_streaming && size >= BYTES_PER_FRAME) {
		s_packets++;
		s_bytes += size;
		(void)i2s_out_write(buf, size / BYTES_PER_FRAME, BYTES_PER_SAMPLE);
	}
	k_mem_slab_free(&rx_slab, buf);
}

static void dac_buf_release_cb(const struct device *dev, uint8_t terminal,
			       void *buf, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(terminal);
	ARG_UNUSED(user_data);

	k_mem_slab_free(&rx_slab, buf);
}

static uint32_t dac_feedback_cb(const struct device *dev, uint8_t terminal, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(terminal);
	ARG_UNUSED(user_data);

	return s_fb_value;
}

static uint32_t dac_get_sample_rate(const struct device *dev, uint8_t clock_id, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(clock_id);
	ARG_UNUSED(user_data);

	return s_rate_hz;
}

static int dac_set_sample_rate(const struct device *dev, uint8_t clock_id, uint32_t rate,
			       void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(clock_id);
	ARG_UNUSED(user_data);

	if (rate != 44100U && rate != 48000U && rate != 96000U) {
		return -EINVAL;
	}
	if (rate != s_rate_hz) {
		/* Reprogramming the PLL and the codec takes milliseconds: do it in the
		 * main thread, not in the USB stack's request context. */
		s_pending_rate = rate;
	}
	return 0;
}

static const struct uac2_ops dac_ops = {
	.sof_cb = dac_sof_cb,
	.terminal_update_cb = dac_terminal_update_cb,
	.get_recv_buf = dac_get_recv_buf,
	.data_recv_cb = dac_data_recv_cb,
	.buf_release_cb = dac_buf_release_cb,
	.feedback_cb = dac_feedback_cb,
	.get_sample_rate = dac_get_sample_rate,
	.set_sample_rate = dac_set_sample_rate,
};

static void usbd_msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
	ARG_UNUSED(ctx);

	printk("[USB] event: %s\n", usbd_msg_type_string(msg->type));
}

/* ---------------------------------------------------------------------------------
 * LCD status screen
 * --------------------------------------------------------------------------------- */
static void ui_static(void)
{
	lcd_clear(LCD_COLOR_BLACK);
	lcd_fill_rect(0, 0, LCD_WIDTH - 1, 27, LCD_COLOR_NAVY);
	lcd_show_string(68, 6, "SPARK USB DAC", LCD_COLOR_CYAN, LCD_COLOR_NAVY);
	lcd_show_string(8, 40, "Rate", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
	lcd_show_string(8, 64, "Format", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
	lcd_show_string(8, 88, "USB", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
	lcd_show_string(8, 112, "Volume", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
	lcd_show_string(8, 136, "Buffer", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
	lcd_show_string(8, 160, "Stereo", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
	lcd_show_string(8, 188, "L", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
	lcd_show_string(8, 212, "R", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
}

static void ui_bar(uint16_t y, uint32_t value, uint32_t full, uint16_t color)
{
	const uint16_t x0 = 28, x1 = 230;
	uint32_t span = (uint32_t)(x1 - x0);
	uint32_t w = (full > 0U) ? (value * span) / full : 0U;

	if (w > span) {
		w = span;
	}
	if (w > 0U) {
		lcd_fill_rect(x0, y, (uint16_t)(x0 + w), (uint16_t)(y + 10), color);
	}
	if (x0 + w < x1) {
		lcd_fill_rect((uint16_t)(x0 + w + 1U), y, x1, (uint16_t)(y + 10), LCD_COLOR_DARKGREY);
	}
}

static void ui_update(void)
{
	char line[28];
	uint16_t pk_l, pk_r;

	snprintf(line, sizeof(line), "%u.%u kHz   ", (unsigned)(s_rate_hz / 1000U),
		 (unsigned)((s_rate_hz % 1000U) / 100U));
	lcd_show_string(88, 40, line, LCD_COLOR_WHITE, LCD_COLOR_BLACK);
	lcd_show_string(88, 64, "24-bit stereo", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
	lcd_show_string(88, 88, s_streaming ? "streaming  " : "idle       ",
			s_streaming ? LCD_COLOR_GREEN : LCD_COLOR_YELLOW, LCD_COLOR_BLACK);

	if (s_muted) {
		snprintf(line, sizeof(line), "MUTED         ");
	} else {
		snprintf(line, sizeof(line), "-%u.%u dB      ", (unsigned)(s_atten / 2U),
			 (unsigned)((s_atten % 2U) * 5U));
	}
	lcd_show_string(88, 112, line, s_muted ? LCD_COLOR_RED : LCD_COLOR_WHITE, LCD_COLOR_BLACK);

	uint32_t lvl = i2s_out_level(), cap = i2s_out_capacity();
	snprintf(line, sizeof(line), "%3u%%  ", (unsigned)((lvl * 100U) / cap));
	lcd_show_string(88, 136, line, LCD_COLOR_WHITE, LCD_COLOR_BLACK);

	if (s_se == 0U) {
		snprintf(line, sizeof(line), "normal      ");
	} else {
		snprintf(line, sizeof(line), "wide %u/7    ", (unsigned)s_se);
	}
	lcd_show_string(88, 160, line, s_se ? LCD_COLOR_CYAN : LCD_COLOR_WHITE, LCD_COLOR_BLACK);

	i2s_out_take_peaks(&pk_l, &pk_r);
	ui_bar(190, pk_l, 32767U, (pk_l > 29000U) ? LCD_COLOR_RED : LCD_COLOR_GREEN);
	ui_bar(214, pk_r, 32767U, (pk_r > 29000U) ? LCD_COLOR_RED : LCD_COLOR_GREEN);
}

int main(void)
{
	struct usbd_context *usbd;
	int err;

	printk("\n=== Spark USB DAC: UAC2 -> circular-DMA I2S3 -> ES8388 ===\n");

	lcd_st7789_init();
	ui_static();
	buttons_init();

	if (es8388_init(s_rate_hz) != 0) {
		lcd_show_string(8, 160, "ES8388 not found!       ", LCD_COLOR_RED, LCD_COLOR_BLACK);
	}
	es8388_set_attenuation(s_atten);

	err = i2s_out_init(s_rate_hz);
	printk("[init] I2S3 circular DMA %s at %u Hz\n", err ? "FAILED" : "running",
	       (unsigned)s_rate_hz);

	if (!device_is_ready(uac2_dev)) {
		printk("[init] UAC2 device not ready\n");
		return 0;
	}
	s_fb_value = feedback_for_level(s_rate_hz, i2s_out_capacity() / 2U);
	usbd_uac2_set_ops(uac2_dev, &dac_ops, NULL);

	usbd = sample_usbd_init_device(usbd_msg_cb);
	if (usbd == NULL || usbd_enable(usbd) != 0) {
		printk("[USB] init/enable failed\n");
		return -ENODEV;
	}
	printk("[USB] enabled\n");

	uint32_t last_ui = 0, last_log = 0;

	while (1) {
		uint32_t now = k_uptime_get_32();

		if (s_pending_rate != 0U) {
			uint32_t r = s_pending_rate;

			s_pending_rate = 0U;
			es8388_set_rate(r);
			(void)i2s_out_set_rate(r);
			s_rate_hz = r;
			printk("[audio] sample rate -> %u Hz\n", (unsigned)r);
		}

		button_id_t b = buttons_poll();
		if (b == BTN_UP && s_atten >= 4U) {
			s_atten -= 4U;                       /* +2 dB */
			es8388_set_attenuation(s_atten);
		} else if (b == BTN_DOWN && s_atten <= 188U) {
			s_atten += 4U;                       /* -2 dB */
			es8388_set_attenuation(s_atten);
		} else if ((b == BTN_RIGHT && s_se < 7U) || (b == BTN_LEFT && s_se > 0U)) {
			s_se = (b == BTN_RIGHT) ? s_se + 1U : s_se - 1U;
			uint8_t rb = es8388_set_stereo_enhance(s_se);
			printk("[audio] stereo enhance %u (reg 0x1D = 0x%02X)\n", (unsigned)s_se, rb);
		} else if (b == BTN_CENTER) {
			s_muted = !s_muted;
			es8388_mute(s_muted);
		}

		if (now - last_ui >= 100U) {
			last_ui = now;
			ui_update();
		}

		if (now - last_log >= 5000U) {
			uint32_t ur, ovr;

			last_log = now;
			i2s_out_get_stats(&ur, &ovr);
			printk("[USB] %s %u Hz pkts=%u bytes=%u level=%u/%u underrun=%u overrun=%u "
			       "fb=%u.%04u iso_out=%u iso_in=%u slab_miss=%u\n",
			       s_streaming ? "OPEN" : "idle", (unsigned)s_rate_hz,
			       (unsigned)s_packets, (unsigned)s_bytes, (unsigned)i2s_out_level(),
			       (unsigned)i2s_out_capacity(), (unsigned)ur, (unsigned)ovr,
			       (unsigned)(s_fb_value >> 14),
			       (unsigned)(((s_fb_value & 0x3FFFU) * 10000U) >> 14),
			       (unsigned)g_iso_out_rearms, (unsigned)g_iso_in_rearms,
			       (unsigned)s_slab_misses);
		}
		k_msleep(10);
	}
	return 0;
}
