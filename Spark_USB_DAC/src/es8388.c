/**
 * @file es8388.c
 * @brief ES8388 codec driver, bit-banged I2C on PF0 (SDA) / PF1 (SCL).
 *
 * Ported from Spark_AIrplay_1/src/audio_codec_es8388.c. The register values are the
 * ones that work on this board; where they differ from the example sequence in the
 * ES8388 user guide (registers 0x00 and 0x01) the working values were kept.
 *
 * Changes for the USB DAC (register meanings from the ES8388 user guide):
 *   0x17 = 0x00   DAC: 24-bit, I2S (DACWL=000, DACFORMAT=00)          [guide 6.3.1]
 *   0x18          DAC single speed (0x02) at <= 48 kHz, double speed (0x22) at 96 kHz;
 *                 ratio field 00010 = 256, which slave mode auto-detects   [guide 5.2]
 *   0x19          DAC mute with soft ramp; 0x32 = unmuted + ramp enabled   [guide 6.3.3]
 *   0x1A/0x1B     DAC digital volume -96..0 dB in 0.5 dB steps (hardware volume)
 */

#include "es8388.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <stm32f4xx.h>

#define SDA_BIT 0U
#define SCL_BIT 1U

static uint8_t s_addr = 0x10;      /* 7-bit address: 0x10 when the CE pin is low */

static void gpio_init(void)
{
	RCC->AHB1ENR |= RCC_AHB1ENR_GPIOFEN;

	/* PF0/PF1: open-drain outputs with pull-ups, idle high */
	GPIOF->MODER = (GPIOF->MODER & ~((3U << (SDA_BIT * 2)) | (3U << (SCL_BIT * 2)))) |
		       ((1U << (SDA_BIT * 2)) | (1U << (SCL_BIT * 2)));
	GPIOF->OTYPER |= (1U << SDA_BIT) | (1U << SCL_BIT);
	GPIOF->OSPEEDR |= (3U << (SDA_BIT * 2)) | (3U << (SCL_BIT * 2));
	GPIOF->PUPDR = (GPIOF->PUPDR & ~((3U << (SDA_BIT * 2)) | (3U << (SCL_BIT * 2)))) |
		       ((1U << (SDA_BIT * 2)) | (1U << (SCL_BIT * 2)));
	GPIOF->BSRR = (1U << SDA_BIT) | (1U << SCL_BIT);
}

static inline void bit_delay(void)
{
	for (volatile int i = 0; i < 200; i++) {
		__NOP();
	}
}

static void i2c_start(void)
{
	GPIOF->BSRR = (1U << SDA_BIT) | (1U << SCL_BIT);
	bit_delay();
	GPIOF->BSRR = (1U << (SDA_BIT + 16));
	bit_delay();
	GPIOF->BSRR = (1U << (SCL_BIT + 16));
	bit_delay();
}

static void i2c_stop(void)
{
	GPIOF->BSRR = (1U << (SDA_BIT + 16));
	bit_delay();
	GPIOF->BSRR = (1U << SCL_BIT);
	bit_delay();
	GPIOF->BSRR = (1U << SDA_BIT);
	bit_delay();
}

static bool i2c_write_byte(uint8_t byte)
{
	for (int i = 7; i >= 0; i--) {
		if (byte & (1U << i)) {
			GPIOF->BSRR = (1U << SDA_BIT);
		} else {
			GPIOF->BSRR = (1U << (SDA_BIT + 16));
		}
		bit_delay();
		GPIOF->BSRR = (1U << SCL_BIT);
		bit_delay();
		GPIOF->BSRR = (1U << (SCL_BIT + 16));
		bit_delay();
	}

	/* ACK clock: release SDA, pulse SCL, sample */
	GPIOF->BSRR = (1U << SDA_BIT);
	bit_delay();
	GPIOF->BSRR = (1U << SCL_BIT);
	bit_delay();
	bool ack = ((GPIOF->IDR & (1U << SDA_BIT)) == 0);
	GPIOF->BSRR = (1U << (SCL_BIT + 16));
	bit_delay();
	return ack;
}

static uint8_t i2c_read_byte(bool send_ack)
{
	uint8_t byte = 0;

	GPIOF->BSRR = (1U << SDA_BIT);
	for (int i = 7; i >= 0; i--) {
		bit_delay();
		GPIOF->BSRR = (1U << SCL_BIT);
		bit_delay();
		if (GPIOF->IDR & (1U << SDA_BIT)) {
			byte |= (1U << i);
		}
		GPIOF->BSRR = (1U << (SCL_BIT + 16));
		bit_delay();
	}

	if (send_ack) {
		GPIOF->BSRR = (1U << (SDA_BIT + 16));
	} else {
		GPIOF->BSRR = (1U << SDA_BIT);
	}
	bit_delay();
	GPIOF->BSRR = (1U << SCL_BIT);
	bit_delay();
	GPIOF->BSRR = (1U << (SCL_BIT + 16));
	GPIOF->BSRR = (1U << SDA_BIT);
	bit_delay();
	return byte;
}

static bool reg_write(uint8_t reg, uint8_t val)
{
	i2c_start();
	bool a0 = i2c_write_byte((uint8_t)(s_addr << 1));
	bool a1 = i2c_write_byte(reg);
	bool a2 = i2c_write_byte(val);
	i2c_stop();
	return a0 && a1 && a2;
}

static uint8_t reg_read(uint8_t reg)
{
	i2c_start();
	if (!i2c_write_byte((uint8_t)(s_addr << 1)) || !i2c_write_byte(reg)) {
		i2c_stop();
		return 0xFF;
	}
	i2c_start();
	if (!i2c_write_byte((uint8_t)((s_addr << 1) | 1U))) {
		i2c_stop();
		return 0xFF;
	}
	uint8_t v = i2c_read_byte(false);
	i2c_stop();
	return v;
}

static uint8_t speed_reg(uint32_t rate_hz)
{
	/* bit 5: double speed for 96 kHz; bits 4:0 = 00010 -> MCLK/LRCK 256 */
	return (rate_hz > 48000U) ? 0x22 : 0x02;
}

int es8388_init(uint32_t rate_hz)
{
	gpio_init();
	k_msleep(20);

	/* Probe both possible addresses (CE low -> 0x10, CE high -> 0x11) */
	bool found = false;
	for (uint8_t addr = 0x10; addr <= 0x11 && !found; addr++) {
		s_addr = addr;
		i2c_start();
		found = i2c_write_byte((uint8_t)(addr << 1));
		i2c_stop();
		k_msleep(10);
	}
	if (!found) {
		printk("[ES8388] no acknowledge on I2C 0x10/0x11\n");
		return -ENODEV;
	}
	printk("[ES8388] codec acknowledged on I2C address 0x%02X\n", s_addr);

	reg_write(0x19, 0x04); /* mute DAC during setup */
	reg_write(0x01, 0x50); /* chip power management */
	reg_write(0x02, 0x00); /* normal */
	reg_write(0x08, 0x00); /* I2S slave mode */

	reg_write(0x04, 0xC0); /* DAC disabled for now */
	reg_write(0x00, 0x12); /* play and record mode */
	reg_write(0x17, 0x00); /* DAC: 24-bit, I2S format */
	reg_write(0x18, speed_reg(rate_hz)); /* DAC: single/double speed, MCLK/LRCK 256 */
	reg_write(0x26, 0x00); /* mixer input select */
	reg_write(0x27, 0x9C); /* L DAC to L mixer */
	reg_write(0x2A, 0x9C); /* R DAC to R mixer */
	reg_write(0x2B, 0x80); /* ADC and DAC share LRCK */
	reg_write(0x2D, 0x00);

	reg_write(0x1A, 0x00); /* L DAC digital volume 0 dB */
	reg_write(0x1B, 0x00); /* R DAC digital volume 0 dB */
	reg_write(0x04, 0x3C); /* enable DAC and LOUT/ROUT */

	/* ADC block powered only because the codec derives its clocks through it */
	reg_write(0x03, 0xFF);
	reg_write(0x09, 0xBB);
	reg_write(0x0A, 0x00);
	reg_write(0x0B, 0x02);
	reg_write(0x0C, 0x0D);
	reg_write(0x0D, speed_reg(rate_hz)); /* ADC ratio mirrors the DAC (shared LRCK) */
	reg_write(0x10, 0x00);
	reg_write(0x11, 0x00);
	reg_write(0x03, 0x09);

	reg_write(0x2E, 0x1E); /* LOUT1 0 dB */
	reg_write(0x2F, 0x1E); /* ROUT1 0 dB */

	/* reset the internal state machine */
	reg_write(0x02, 0xF0);
	k_msleep(5);
	reg_write(0x02, 0x00);
	k_msleep(5);

	reg_write(0x04, 0x3C);
	reg_write(0x19, 0x32); /* un-mute, soft volume ramp enabled */
	k_msleep(10);

	uint8_t r4 = reg_read(0x04);
	printk("[ES8388] DAC power reg 0x04 = 0x%02X (%s)\n", r4, r4 == 0x3C ? "ok" : "unexpected");
	return 0;
}

void es8388_set_rate(uint32_t rate_hz)
{
	reg_write(0x19, 0x36); /* mute (soft ramp) while the clock changes */
	reg_write(0x18, speed_reg(rate_hz));
	reg_write(0x0D, speed_reg(rate_hz));
	reg_write(0x02, 0xF0); /* restart the state machine on the new clock */
	k_msleep(2);
	reg_write(0x02, 0x00);
	reg_write(0x19, 0x32);
}

void es8388_set_attenuation(uint8_t half_db_steps)
{
	if (half_db_steps > 192U) {
		half_db_steps = 192U;
	}
	reg_write(0x1A, half_db_steps);
	reg_write(0x1B, half_db_steps);
}

void es8388_mute(bool mute)
{
	reg_write(0x19, mute ? 0x36 : 0x32); /* bit 2 DACMute, bit 5 soft ramp */
}

uint8_t es8388_set_stereo_enhance(uint8_t strength)
{
	if (strength > 7U) {
		strength = 7U;
	}
	/* Reg 0x1D (DAC Control 7): bits 4:2 SE strength. Read-modify-write keeps
	 * ZeroL/ZeroR/Mono and Vpp_scale as they are. Returns the read-back value. */
	uint8_t v = reg_read(0x1D);

	v = (uint8_t)((v & ~0x1CU) | (strength << 2));
	reg_write(0x1D, v);
	return reg_read(0x1D);
}

uint8_t es8388_address(void)
{
	return s_addr;
}
