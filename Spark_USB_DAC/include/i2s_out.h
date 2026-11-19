/**
 * @file i2s_out.h
 * @brief Gap-free I2S3 audio output: one circular DMA buffer fed from a ring buffer.
 *
 * Unlike Zephyr's I2S driver (one DMA job per block, restarted every block), the DMA
 * here is started once and never stops; a half-transfer / transfer-complete interrupt
 * refills the half that just played. The ring-buffer fill level is the clock-drift
 * error signal used for the USB feedback value.
 *
 * Output format: 24-bit samples in 32-bit I2S slots, stereo, MCK = 256 x Fs.
 */

#ifndef I2S_OUT_H_
#define I2S_OUT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Configure pins, PLLI2S for @p rate_hz, and start the circular DMA (silent output). */
int i2s_out_init(uint32_t rate_hz);

/** Re-program PLLI2S and the I2S divider for a new rate (44100, 48000 or 96000). */
int i2s_out_set_rate(uint32_t rate_hz);

/**
 * Append interleaved stereo frames from a USB packet.
 * @param data   packed little-endian samples, @p bytes_per_sample bytes each (2 or 3)
 * @param frames number of stereo frames
 * @return frames accepted (fewer if the ring is full)
 */
size_t i2s_out_write(const uint8_t *data, size_t frames, uint8_t bytes_per_sample);

/** Drop all buffered audio and output silence until enough is buffered again. */
void i2s_out_flush(void);

/** Stereo frames currently buffered in the ring. */
uint32_t i2s_out_level(void);

/** Ring capacity in stereo frames. */
uint32_t i2s_out_capacity(void);

/** Diagnostics: frames output as silence because the ring ran dry, and frames
 *  dropped because the ring was full. */
void i2s_out_get_stats(uint32_t *underrun_frames, uint32_t *overrun_frames);

/** Peak absolute level per channel since the last call, 0..32767 (for VU meters). */
void i2s_out_take_peaks(uint16_t *left, uint16_t *right);

#endif /* I2S_OUT_H_ */
