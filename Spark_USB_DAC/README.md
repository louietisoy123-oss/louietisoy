# Spark USB DAC: USB Type-C to 3.5 mm Sound Card

Turns the **RT-Thread RT-Spark board** (STM32F407ZGT6) into a plug-and-play **USB Audio Class 2** sound card. Plug the board's **USB-DEV Type-C port** into a PC. Windows lists it as **"Headphones (Spark USB DAC)"**, and audio plays through the onboard **ES8388** codec to the 3.5 mm jack. No driver install is needed.

Built on **Zephyr RTOS 4.4** (new USB device stack) with PlatformIO. The I2S3 and DMA output path is written at register level.

| | |
|---|---|
| Formats | **44.1 / 48 / 96 kHz, 24-bit stereo** (PCM, 3-byte subslots) |
| USB | Full speed (12 Mbit/s), UAC2, **asynchronous** isochronous OUT with explicit feedback |
| Output | ES8388 DAC, 24-bit Philips I2S in 32-bit slots, MCLK = 256 × Fs |
| Latency | Ring buffer held at half full: about 21 ms at 96 kHz, about 43 ms at 48 kHz |
| Board UI | ST7789 LCD status screen; D-pad volume, mute and stereo enhancement |
| Footprint | 99 KB flash (9.5 %), 59 KB RAM (45 %) |

---

## 1. Signal path

```
 PC (Windows UAC2 driver)
   │  isochronous OUT, 1 packet / 1 ms        ▲ explicit feedback (Q16.16, 1 per frame)
   ▼                                          │
 OTG FS (PA11/PA12) ── Zephyr usbd + UAC2 class ── feedback regulator (ring level → rate)
   │ data_recv_cb: 24-bit LE → left-justified int32
   ▼
 Ring buffer 4096 stereo frames (src/i2s_out.c)
   │ DMA1 Stream 5 half/complete ISR copies 48 frames at a time
   ▼
 Circular DMA (never stopped) → SPI3/I2S3 master TX, MCK out
   │ PC7 MCK · PB3 BCLK · PA15 LRCK · PB5 SDOUT
   ▼
 ES8388 (slave, 24-bit I2S) → LOUT1/ROUT1 → 3.5 mm jack
```

**Clocking:** The codec clock comes from **PLLI2S** (HSE 8 MHz, PLLM 8). One table entry per rate keeps MCK at 256 × Fs:

| Rate | PLLI2SN | PLLI2SR | I2SDIV / ODD | Actual Fs |
|---|---|---|---|---|
| 44.1 kHz | 271 | 2 | 6 / 0 | 44 108 Hz |
| 48 kHz | 172 | 2 | 3 / 1 | 47 991 Hz |
| 96 kHz | 344 | 2 | 3 / 1 | 95 982 Hz |

The roughly 185 ppm error doesn't matter. The endpoint is asynchronous, so the board reports its real consumption rate and the PC follows it.

**Rate control:** On every USB start-of-frame the firmware computes feedback from the ring fill level. Nominal is `Fs/1000` samples per frame, adjusted by 1/1024 sample per frame for each frame of error away from half full, and clamped to ±1 %. On the bench at 96 kHz the ring holds at about 2050/4095 frames with **0 underruns and 0 overruns**.

---

## 2. Hardware

| Function | Pins | Notes |
|---|---|---|
| USB-DEV Type-C | PA11 (D−), PA12 (D+) | OTG FS device. The *other* Type-C port is the ST-LINK. |
| I2S3 to ES8388 | PC7 MCK, PB3 CK, PA15 WS, PB5 SD (AF6) | DMA1 Stream 5, channel 0 |
| ES8388 control | PF0 SDA, PF1 SCL (bit-banged I2C) | address 0x10 |
| ST7789 LCD | FSMC Bank 1 NE3, 8-bit (PD0/1/4/5/13/14/15, PE7–10), PD3 reset, PF9 backlight | |
| D-pad | PC5 up, PC1 down, PC0 left, PC4 right, PA0 centre | active low |
| Console | USART1 PA9/PA10, 115200 baud | via ST-LINK virtual COM port |

Connect **both** cables: ST-LINK for flashing and logs, USB-DEV for audio.

---

## 3. Build, flash, use

```bash
cd Spark_USB_DAC
pio run -t upload          # build + flash via ST-LINK
pio device monitor         # USART1 log (COM port of the ST-LINK VCP, 115200)
```

1. Plug the **USB-DEV** port into the PC. *Spark USB DAC* appears under Sound → Output.
2. Pick the format in *Sound → Spark USB DAC → Properties → Advanced* (24-bit, 44.1 / 48 / 96 kHz).
3. Play anything. The LCD shows the rate, `streaming`, buffer fill and L/R level meters.

### Board controls

| Button | Action |
|---|---|
| UP / DOWN | Codec DAC volume ±2 dB (registers 0x1A/0x1B, 0.5 dB steps). Starts at −12 dB. |
| CENTER | Mute / unmute (soft ramp, register 0x19) |
| RIGHT / LEFT | **Stereo enhancement** 0 (off) to 7 (register 0x1D bits 4:2). Other bits are kept. |

Zephyr 4.4's UAC2 class doesn't handle feature-unit volume requests, so the Windows volume slider scales the audio in software. The buttons drive the codec's own attenuator.

### Status log (every 5 s)

```
[USB] OPEN 96000 Hz pkts=13948 bytes=8032536 level=2052/4095 underrun=0 overrun=0 fb=95.9951 iso_out=26 iso_in=26 slab_miss=0
```

`level` is the ring fill. `fb` is the feedback sent, in kHz. `iso_out`/`iso_in` count recovered isochronous transfers, which only increase around stream start.

---

## 4. Problems solved along the way

Each item was diagnosed on hardware, from the serial log.

| Symptom | Root cause | Fix |
|---|---|---|
| Windows *Code 10* (device timeout) | USB driver thread stack overflow (512 B default); then a hard fault, PC=0, in `uac2_sof` | Stacks set to 2048 B, deferred logging; mandatory `sof_cb` added |
| Enumerates, stream opens, **0 bytes received** | STM32 HAL drops an isochronous transfer that misses its even/odd frame. Zephyr's `udc_stm32` doesn't implement `HAL_PCD_ISOOUTIncompleteCallback`, so the endpoint stayed dead | `src/usb_iso_fix.c` re-arms the OUT transfer, and the IN (feedback) transfer the same way |
| Windows ignored feedback, so the buffer drifted | Same issue on the feedback IN endpoint | `HAL_PCD_ISOINIncompleteCallback` override |
| No data at 96 kHz / 24-bit | 600 B RxFIFO can't hold a 582 B packet plus the next one | `CONFIG_UDC_STM32_OTG_RXFIFO_BASELINE_SIZE=1152` (fits the 1280 B FIFO RAM budget) |
| Periodic gaps, about 0.9 % shortfall | Zephyr I2S driver restarts DMA for each block | Replaced with a never-stopping circular DMA fed from a ring (`src/i2s_out.c`) |
| **Loud white noise** (data flowing, stats healthy) | Stopping I2S mid-frame on the host's 48→96 kHz switch left half a sample in SPI_DR. After restart every 24-in-32 sample had its 16-bit halves swapped | `i2s_stop()` stops DMA, waits for TXE/!BSY, then resets SPI3 through RCC |
| Old descriptors after a format change | Windows caches UAC descriptors per VID/PID | PID changed with each format change (now `0x0A03`) |

---

## 5. Code map

| File | Role |
|---|---|
| `src/main.c` | UAC2 callbacks, feedback regulator, rate switching, LCD UI, buttons, status log |
| `src/i2s_out.c` | PLLI2S rate table, I2S3 + circular DMA, ring buffer, peak meters |
| `src/es8388.c` | Codec init (24-bit I2S slave, single/double speed), volume, mute, stereo enhancement |
| `src/usb_iso_fix.c` | Incomplete-isochronous recovery for the STM32 OTG HAL |
| `src/usbd_setup.c` | USB device/configuration descriptors (adapted from Zephyr `samples/subsys/usb/common`, Apache-2.0) |
| `src/lcd_st7789.c`, `src/buttons.c` | Shared with `Spark_AIrplay_1` |
| `zephyr/boards/black_f407zg_pro.overlay` | UAC2 topology: clock source, USB streaming terminal, headphone output terminal, 24-bit AS interface |
| `zephyr/prj.conf` | USB stack, FIFO sizing, stack sizes, logging |

---

## 6. Limits

- **Full-speed USB** caps the format: 96 kHz × 24-bit × 2 channels uses about 576 KB/s of the roughly 1 MB/s isochronous budget. 192 kHz or 32-bit would need high-speed USB, which the board's OTG FS port can't do. The ES8388 also tops out at 96 kHz.
- The VID/PID are Zephyr sample values: fine for bench use, not for distribution.
- No microphone or line-in capture path (playback only).
