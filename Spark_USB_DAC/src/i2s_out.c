/**
 * @file i2s_out.c
 * @brief Gap-free I2S3 output for the ES8388: circular DMA (DMA1 Stream 5, ch 0) fed
 *        from a ring buffer. Register-level, ported from the AirPlay project's proven
 *        I2S3 setup (Spark_AIrplay_1/src/audio_codec_es8388.c) and extended to 24-bit
 *        data and three sample rates.
 *
 * Pins (Spark schematic): PA15 WS, PB3 CK, PB5 SD, PC7 MCK, all AF6.
 *
 * Data path: USB packet -> i2s_out_write() -> ring (int32 per channel, left-justified
 * 24-bit) -> DMA ISR copies the next half-buffer as 16-bit halfwords (the SPI data
 * register is 16 bits wide; a 32-bit I2S slot is sent as high half then low half).
 */

#include "i2s_out.h"

#include <errno.h>
#include <string.h>

#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <stm32f4xx.h>

/* Ring: 4096 stereo frames = 85 ms at 48 kHz, 43 ms at 96 kHz. */
#define RING_FRAMES        4096U
#define RING_MASK          (RING_FRAMES - 1U)
/* DMA buffer: 2 halves x 48 frames; each frame = 2 channels x 32-bit = 4 halfwords. */
#define HALF_FRAMES        48U
#define HW_PER_FRAME       4U
#define DMA_HALFWORDS      (2U * HALF_FRAMES * HW_PER_FRAME)
/* Start playing once this much is buffered, and re-prime after an underrun. */
#define PRIME_FRAMES       (RING_FRAMES / 2U)

static int32_t s_ring[RING_FRAMES][2];
static volatile uint32_t s_head;       /* written by USB (thread) */
static volatile uint32_t s_tail;       /* read by DMA ISR */
static volatile bool s_primed;

static uint16_t s_dma_buf[DMA_HALFWORDS] __aligned(4);

static volatile uint32_t s_underrun_frames;
static volatile uint32_t s_overrun_frames;
static volatile uint16_t s_peak_l;
static volatile uint16_t s_peak_r;

/* Rate table: PLLI2S N / R and I2S divider (DIV, ODD) for MCK = 256 x Fs, from an
 * exhaustive search with HSE 8 MHz and PLLM 8 (1 MHz VCO input). Error ~185 ppm on
 * all three; the USB feedback loop absorbs it. */
struct rate_cfg {
	uint32_t hz;
	uint16_t plln;
	uint8_t pllr;
	uint8_t div;
	uint8_t odd;
};

static const struct rate_cfg s_rates[] = {
	{ 44100, 271, 2, 6, 0 },   /* 44,108 Hz */
	{ 48000, 172, 2, 3, 1 },   /* 47,991 Hz */
	{ 96000, 344, 2, 3, 1 },   /* 95,982 Hz */
};

static const struct rate_cfg *find_rate(uint32_t hz)
{
	for (size_t i = 0; i < ARRAY_SIZE(s_rates); i++) {
		if (s_rates[i].hz == hz) {
			return &s_rates[i];
		}
	}
	return NULL;
}

static inline uint32_t ring_level(void)
{
	return (s_head - s_tail) & RING_MASK;
}

/* Fill one DMA half (48 frames) from the ring, or with silence. ISR context. */
static void fill_half(uint16_t *dst)
{
	uint32_t avail = ring_level();
	uint16_t pk_l = 0, pk_r = 0;

	if (!s_primed && avail >= PRIME_FRAMES) {
		s_primed = true;
	}

	for (uint32_t i = 0; i < HALF_FRAMES; i++) {
		int32_t l = 0, r = 0;

		if (s_primed && avail > 0U) {
			l = s_ring[s_tail][0];
			r = s_ring[s_tail][1];
			s_tail = (s_tail + 1U) & RING_MASK;
			avail--;
		} else if (s_primed) {
			/* Ran dry: play silence and wait until half the ring is full again */
			s_primed = false;
			s_underrun_frames += HALF_FRAMES - i;
		}

		/* 32-bit slot, MSB first: high halfword then low halfword */
		dst[0] = (uint16_t)((uint32_t)l >> 16);
		dst[1] = (uint16_t)((uint32_t)l & 0xFFFFU);
		dst[2] = (uint16_t)((uint32_t)r >> 16);
		dst[3] = (uint16_t)((uint32_t)r & 0xFFFFU);
		dst += HW_PER_FRAME;

		uint16_t al = (uint16_t)(((l < 0) ? -(l >> 16) : (l >> 16)) & 0x7FFF);
		uint16_t ar = (uint16_t)(((r < 0) ? -(r >> 16) : (r >> 16)) & 0x7FFF);
		if (al > pk_l) {
			pk_l = al;
		}
		if (ar > pk_r) {
			pk_r = ar;
		}
	}
	if (pk_l > s_peak_l) {
		s_peak_l = pk_l;
	}
	if (pk_r > s_peak_r) {
		s_peak_r = pk_r;
	}
}

static void dma1_stream5_isr(const void *arg)
{
	ARG_UNUSED(arg);
	uint32_t st = DMA1->HISR;

	if (st & DMA_HISR_HTIF5) {
		DMA1->HIFCR = DMA_HIFCR_CHTIF5;
		fill_half(&s_dma_buf[0]);                         /* first half just played */
	}
	if (st & DMA_HISR_TCIF5) {
		DMA1->HIFCR = DMA_HIFCR_CTCIF5;
		fill_half(&s_dma_buf[DMA_HALFWORDS / 2U]);        /* second half just played */
	}
	if (st & (DMA_HISR_TEIF5 | DMA_HISR_DMEIF5 | DMA_HISR_FEIF5)) {
		DMA1->HIFCR = DMA_HIFCR_CTEIF5 | DMA_HIFCR_CDMEIF5 | DMA_HIFCR_CFEIF5;
	}
}

static void pins_init(void)
{
	RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOCEN;

	/* AF6 on PC7 (MCK), PA15 (WS), PB3 (CK), PB5 (SD), very high speed */
	GPIOC->MODER = (GPIOC->MODER & ~(3U << (7 * 2))) | (2U << (7 * 2));
	GPIOC->AFR[0] = (GPIOC->AFR[0] & ~(0xFU << (7 * 4))) | (6U << (7 * 4));
	GPIOC->OSPEEDR |= (3U << (7 * 2));

	GPIOA->MODER = (GPIOA->MODER & ~(3U << (15 * 2))) | (2U << (15 * 2));
	GPIOA->AFR[1] = (GPIOA->AFR[1] & ~(0xFU << ((15 - 8) * 4))) | (6U << ((15 - 8) * 4));
	GPIOA->OSPEEDR |= (3U << (15 * 2));

	GPIOB->MODER = (GPIOB->MODER & ~((3U << (3 * 2)) | (3U << (5 * 2)))) |
		       ((2U << (3 * 2)) | (2U << (5 * 2)));
	GPIOB->AFR[0] = (GPIOB->AFR[0] & ~((0xFU << (3 * 4)) | (0xFU << (5 * 4)))) |
			((6U << (3 * 4)) | (6U << (5 * 4)));
	GPIOB->OSPEEDR |= (3U << (3 * 2)) | (3U << (5 * 2));
}

static int pll_set(const struct rate_cfg *rc)
{
	RCC->CFGR &= ~RCC_CFGR_I2SSRC;                     /* I2S clock from PLLI2S */
	RCC->CR &= ~RCC_CR_PLLI2SON;
	while (RCC->CR & RCC_CR_PLLI2SRDY) {
	}
	RCC->PLLI2SCFGR = ((uint32_t)rc->plln << RCC_PLLI2SCFGR_PLLI2SN_Pos) |
			  ((uint32_t)rc->pllr << RCC_PLLI2SCFGR_PLLI2SR_Pos);
	RCC->CR |= RCC_CR_PLLI2SON;

	for (uint32_t t = 0; t < 1000000U; t++) {
		if (RCC->CR & RCC_CR_PLLI2SRDY) {
			return 0;
		}
	}
	return -ETIMEDOUT;
}

/*
 * Stop cleanly, leaving no half-sample behind.
 *
 * A 24-bit sample in a 32-bit slot is two 16-bit writes to SPI_DR (high half, then
 * low half). Disabling the I2S mid-stream can leave one half-word sitting in DR; on
 * the next start that stale half goes out first, every later sample is shifted by
 * one half-word, and each sample plays with its halves swapped. Music's low byte is
 * near random, so the result is loud white noise.
 *
 * So: stop feeding DR, let the I2S drain, then reset the whole SPI3 block through
 * RCC so its data register and channel-side state start from zero. i2s_start()
 * rewrites every register afterwards.
 */
static void i2s_stop(void)
{
	DMA1_Stream5->CR &= ~DMA_SxCR_EN;                  /* no more writes to DR */
	while (DMA1_Stream5->CR & DMA_SxCR_EN) {
	}
	SPI3->CR2 &= ~SPI_CR2_TXDMAEN;

	if (SPI3->I2SCFGR & SPI_I2SCFGR_I2SE) {
		/* RM0090: wait for TXE = 1 and BSY = 0 before clearing I2SE */
		for (uint32_t t = 0; t < 100000U; t++) {
			if ((SPI3->SR & SPI_SR_TXE) && !(SPI3->SR & SPI_SR_BSY)) {
				break;
			}
		}
		SPI3->I2SCFGR &= ~SPI_I2SCFGR_I2SE;
	}

	RCC->APB1RSTR |= RCC_APB1RSTR_SPI3RST;             /* clear DR and frame state */
	RCC->APB1RSTR &= ~RCC_APB1RSTR_SPI3RST;

	DMA1->HIFCR = DMA_HIFCR_CFEIF5 | DMA_HIFCR_CDMEIF5 | DMA_HIFCR_CTEIF5 |
		      DMA_HIFCR_CHTIF5 | DMA_HIFCR_CTCIF5;
}

static void i2s_start(const struct rate_cfg *rc)
{
	/* I2S master transmit, Philips standard, 24-bit data in 32-bit channel, MCK on */
	SPI3->I2SCFGR = 0;
	SPI3->I2SPR = SPI_I2SPR_MCKOE | (rc->odd ? SPI_I2SPR_ODD : 0U) | rc->div;
	SPI3->I2SCFGR = SPI_I2SCFGR_I2SMOD | SPI_I2SCFGR_I2SCFG_1 |  /* master TX */
			SPI_I2SCFGR_DATLEN_0 | SPI_I2SCFGR_CHLEN;     /* 24-bit / 32-bit */

	memset(s_dma_buf, 0, sizeof(s_dma_buf));
	DMA1_Stream5->PAR = (uint32_t)&SPI3->DR;
	DMA1_Stream5->M0AR = (uint32_t)s_dma_buf;
	DMA1_Stream5->NDTR = DMA_HALFWORDS;
	DMA1_Stream5->FCR = 0;                             /* direct mode */
	DMA1_Stream5->CR = (0U << DMA_SxCR_CHSEL_Pos) |    /* SPI3_TX = channel 0 */
			   DMA_SxCR_PL_1 | DMA_SxCR_MINC | DMA_SxCR_CIRC |
			   DMA_SxCR_DIR_0 | DMA_SxCR_PSIZE_0 | DMA_SxCR_MSIZE_0 |
			   DMA_SxCR_HTIE | DMA_SxCR_TCIE;

	DMA1_Stream5->CR |= DMA_SxCR_EN;
	SPI3->CR2 |= SPI_CR2_TXDMAEN;
	SPI3->I2SCFGR |= SPI_I2SCFGR_I2SE;
}

int i2s_out_init(uint32_t rate_hz)
{
	const struct rate_cfg *rc = find_rate(rate_hz);

	if (rc == NULL) {
		return -EINVAL;
	}

	RCC->APB1ENR |= RCC_APB1ENR_SPI3EN;
	RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;
	pins_init();

	if (pll_set(rc) != 0) {
		return -ETIMEDOUT;
	}

	i2s_stop();
	IRQ_CONNECT(DMA1_Stream5_IRQn, 0, dma1_stream5_isr, NULL, 0);
	irq_enable(DMA1_Stream5_IRQn);
	i2s_start(rc);
	return 0;
}

int i2s_out_set_rate(uint32_t rate_hz)
{
	const struct rate_cfg *rc = find_rate(rate_hz);

	if (rc == NULL) {
		return -EINVAL;
	}
	i2s_stop();
	i2s_out_flush();
	if (pll_set(rc) != 0) {
		return -ETIMEDOUT;
	}
	i2s_start(rc);
	return 0;
}

size_t i2s_out_write(const uint8_t *data, size_t frames, uint8_t bytes_per_sample)
{
	size_t done = 0;

	while (done < frames) {
		uint32_t head = s_head;
		uint32_t next = (head + 1U) & RING_MASK;

		if (next == s_tail) {
			s_overrun_frames += (uint32_t)(frames - done);
			break;
		}

		int32_t ch[2];
		for (int c = 0; c < 2; c++) {
			if (bytes_per_sample == 3U) {
				/* 24-bit little-endian -> left-justified 32-bit */
				ch[c] = (int32_t)(((uint32_t)data[0] << 8) | ((uint32_t)data[1] << 16) |
						  ((uint32_t)data[2] << 24));
			} else {
				/* 16-bit little-endian -> left-justified 32-bit */
				ch[c] = (int32_t)(((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 24));
			}
			data += bytes_per_sample;
		}
		s_ring[head][0] = ch[0];
		s_ring[head][1] = ch[1];
		s_head = next;
		done++;
	}
	return done;
}

void i2s_out_flush(void)
{
	unsigned int key = irq_lock();

	s_head = 0;
	s_tail = 0;
	s_primed = false;
	irq_unlock(key);
}

uint32_t i2s_out_level(void)
{
	return ring_level();
}

uint32_t i2s_out_capacity(void)
{
	return RING_FRAMES - 1U;
}

void i2s_out_get_stats(uint32_t *underrun_frames, uint32_t *overrun_frames)
{
	if (underrun_frames) {
		*underrun_frames = s_underrun_frames;
	}
	if (overrun_frames) {
		*overrun_frames = s_overrun_frames;
	}
}

void i2s_out_take_peaks(uint16_t *left, uint16_t *right)
{
	unsigned int key = irq_lock();

	*left = s_peak_l;
	*right = s_peak_r;
	s_peak_l = 0;
	s_peak_r = 0;
	irq_unlock(key);
}
