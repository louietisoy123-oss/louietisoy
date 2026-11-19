/**
 * @file es8388.h
 * @brief ES8388 stereo codec driver for the Spark board (playback only).
 *
 * Control: bit-banged I2C on PF0 (SDA) / PF1 (SCL). The codec is the I2S slave and
 * auto-detects MCLK/LRCK in slave mode (ES8388 user guide, section 5.2). Audio format:
 * 24-bit Philips I2S in 32-bit slots, matching i2s_out.c.
 */

#ifndef ES8388_H_
#define ES8388_H_

#include <stdbool.h>
#include <stdint.h>

/** Probe and configure for 24-bit I2S playback at @p rate_hz. 0, or -ENODEV. */
int es8388_init(uint32_t rate_hz);

/** Select single speed (<= 48 kHz) or double speed (96 kHz) for a new rate. */
void es8388_set_rate(uint32_t rate_hz);

/**
 * DAC digital attenuation in 0.5 dB steps (register 0x1A/0x1B):
 * 0 = 0 dB (full scale), 192 = -96 dB. Used as the board's volume control.
 */
void es8388_set_attenuation(uint8_t half_db_steps);

/** Mute or un-mute the DAC (soft-ramped, register 0x19). */
void es8388_mute(bool mute);

/**
 * Stereo enhancement strength 0 (off) to 7 (register 0x1D bits 4:2), applied to
 * the DAC path. Other bits of 0x1D are preserved. Returns the register read-back.
 */
uint8_t es8388_set_stereo_enhance(uint8_t strength);

/** Codec I2C address found at init (0x10 or 0x11), 0 if not found. */
uint8_t es8388_address(void);

#endif /* ES8388_H_ */
