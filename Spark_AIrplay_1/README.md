# RT-Spark AirPlay 1 (RAOP) Hi-Fi Audio Receiver

An embedded **AirPlay 1 (RAOP)** wireless audio receiver engineered for the **RT-Thread RT-Spark Development Board** (STM32F407ZGT6) running **Zephyr RTOS**. It utilizes the **onboard Realtek RW007 SPI Wi-Fi module** (without requiring an external ESP32 coprocessor) to deliver 44.1 kHz 16-bit CD-quality stereo audio streaming directly from Apple devices (iPhone, iPad, Mac, and iTunes).

---

## 1. System Architecture

```
                                  [ Apple Client (iOS / macOS / iTunes) ]
                                                     │
                                       mDNS (_raop._tcp on port 5000)
                                                     │
                                       RTSP (Port 5000: ANNOUNCE, SETUP, RECORD)
                                                     │
                                                     ▼
                             ┌───────────────────────────────────────────────┐
                             │    RT-Spark (STM32F407ZGT6 @ 168 MHz)         │
                             │                                               │
                             │  ┌─────────────────────────────────────────┐  │
                             │  │         Realtek RW007 SPI Wi-Fi         │  │
                             │  │       (SPI2 + Zephyr Net L2 / BSD)      │  │
                             │  └────────────────────┬────────────────────┘  │
                             │                       │                       │
                             │  ┌────────────────────▼────────────────────┐  │
                             │  │     RAOP Protocol Engine (TCP/UDP)      │  │
                             │  │  - mDNS Multicast Responder (5353)      │  │
                             │  │  - RTSP Control Server (Port 5000)      │  │
                             │  │  - RSA-2048 Challenge & AES Decryption  │  │
                             │  │  - RTP Audio & Jitter Reorder Buffer    │  │
                             │  │  - ALAC (Apple Lossless) Decoder        │  │
                             │  └────────────────────┬────────────────────┘  │
                             │                       │                       │
                             │  ┌────────────────────▼────────────────────┐  │
                             │  │      Audio Subsystem (44.1kHz Stereo)   │  │
                             │  │   - DTCM Circular Ring Buffer (64 KB)   │  │
                             │  │   - ES8388 Codec Driver (I2S3 + DMA)    │  │
                             │  └────────┬──────────────────────┬─────────┘  │
                             │           │                      │            │
                             └───────────┼──────────────────────┼────────────┘
                                         ▼                      ▼
                            [ 3.5mm Headphone Jack ]   [ ST7789 IPS LCD (240x240) ]
                                                       [ SK6805 19-LED VU Visualizer]
```

---

## 2. Key Features

- **Full AirPlay 1 (RAOP) Protocol Engine**:
  - **mDNS Service Discovery**: Advertises `_raop._tcp` service on port 5000 with hardware MAC address (`<MAC>@RT-Spark Hi-Fi`).
  - **RTSP Handshake**: Implements `OPTIONS`, `ANNOUNCE`, `SETUP`, `RECORD`, `SET_PARAMETER`, `FLUSH`, and `TEARDOWN`.
  - **Apple-Challenge Authentication**: Native RSA-2048 PKCS#1 v1.5 private key signing.
  - **Session Key Decryption**: RSA-2048 OAEP decryption of the 128-bit AES master audio key.
  - **RTP Packet Reception & Jitter Buffer**: Real-time UDP streaming on dynamic ports with out-of-order reordering, packet loss concealment, and retransmit requests (0xD5/0xD6).
  - **ALAC Decoding**: Real-time software decompression of Apple Lossless audio frames into raw 16-bit 44.1 kHz stereo PCM.
- **Dedicated Realtek RW007 SPI Wi-Fi Module**:
  - Fully integrated SPI slave driver on SPI2 with hardware INT and RESET lines.
  - Zephyr L2 Ethernet emulation with BSD Sockets API and IGMP multicast support.
- **Hardware Audio Output**:
  - Everest Semiconductor **ES8388** high-fidelity stereo codec over I2S3 with double-buffered circular DMA (DMA1 Stream 5).
  - Hardware line-out / 3.5mm headphone amplifier with smooth dB attenuation curve matching Apple volume levels.
- **Visual Telemetry & Visualizer**:
  - **ST7789 240x240 IPS LCD**: Displays connection state, Wi-Fi SSID/IP, active track metadata (Title, Artist, Album via DMAP), and real-time left/right peak VU meters.
  - **19-LED SK6805 RGB Ring Visualizer**: Audio envelope follower and reactive color spectrum animation.

---

## 3. Hardware Pinout & Wiring

| Subsystem | Signal | STM32F407 Pin | Description |
|---|---|---|---|
| **RW007 Wi-Fi** | SPI2 SCK | `PB13` | SPI Clock |
| | SPI2 MISO | `PC2` | Master In / Slave Out |
| | SPI2 MOSI | `PC3` | Master Out / Slave In |
| | SPI2 CS | `PG15` | Slave Select (Active Low) |
| | RW007 INT | `PF10` | Interrupt / Data Ready |
| | RW007 RST | `PG11` | Hardware Reset |
| **ES8388 Audio** | I2S3 WS (LRCK) | `PA15` | Word Select (Left/Right Clock) |
| | I2S3 CK (BCLK) | `PB3` | Bit Clock |
| | I2S3 SD (DIN) | `PB5` | Serial Data Out (to ES8388 DAC) |
| | I2S3 MCK | `PC7` | Master Clock |
| | I2C1 SCL | `PB6` | Codec Configuration Clock |
| | I2C1 SDA | `PB7` | Codec Configuration Data |
| **ST7789 LCD** | LCD D0..D15 | `PD14,15,0,1, PE7..15` | FSMC 16-bit Parallel Data Bus |
| | LCD RS / CS | `PD11 / PD7` | Register Select / Chip Select |
| | LCD WR / RD | `PD5 / PD4` | Write Enable / Read Enable |
| | LCD RST / BL | `PD12 / PB15` | Reset / Backlight PWM |
| **RGB LED** | SK6805 DIN | `PA8` | 19-LED RGB Visualizer Data |
| **Buttons** | D-Pad / Keys | `PA0, PC13, PB10..11` | 5-way navigation & playback control |

---

## 4. Software Stack & Memory Architecture

- **Operating System**: Zephyr RTOS v4.4.0 (L2 Ethernet, BSD Sockets, IGMP, Multicast DNS).
- **Crypto Suite**: Vendored standalone mbedTLS 3.6 cryptographic primitives (`aes`, `rsa`, `bignum`, `sha1`, `md`, `asn1parse`, `pkparse`) compiled directly into `src/crypto/` to eliminate TF-PSA-Crypto conflicts.
- **DTCM Placement (64 KB @ 0x10000000)**:
  - 16-bit stereo PCM circular DMA audio ring buffer (`s_pcm_ring`).
  - RW007 SPI high-speed frame transfer buffers (`s_tx_frame`, `s_rx_frame`).
- **Internal SRAM (128 KB)**:
  - Zephyr kernel threads, networking buffers, RTP reorder window (12 packets, ~18 KB), and ALAC decoding state buffers.

---

## 5. Building & Flashing

### Prerequisites
- [PlatformIO Core](https://platformio.org/) installed.
- ST-LINK V2/V3 programmer connected to the RT-Spark SWD header (or onboard ST-LINK VCP).

### Build Commands
```bash
# Navigate to the project directory
cd Spark_AIrplay_1

# Build the firmware
pio run

# Flash to RT-Spark board via ST-LINK
pio run -t upload
```

---

## 6. How to Connect to Wi-Fi

The system provides three connection options:

### Method A: Web Captive Portal (Easiest & Recommended)
Whenever the RT-Spark board boots with no saved Wi-Fi credentials (or if you **hold the D-Pad CENTER button during boot**), it automatically launches an onboarding **Captive Portal**:
1. **Connect your Phone or PC**:
   - Open Wi-Fi settings on your iPhone, iPad, Android, or laptop and connect to:
     - **SSID**: `RT-Spark-Setup` (Open network, no password required)
2. **Web Portal Popup**:
   - iOS, macOS, Android, and Windows will automatically pop up the **"RT-Spark AirPlay Setup"** dialog via wildcard DNS (UDP port 53).
   - Alternatively, open any browser and navigate to `http://192.168.4.1`.
3. **Select and Save**:
   - The portal performs a live scan of nearby Wi-Fi networks and populates a dropdown with SSIDs and dBm signal strengths.
   - Select your network from the dropdown (or enter it manually) and type the password.
   - Tap **"Save & Connect"**.
4. **Permanent Storage**:
   - The board saves the credentials into on-chip Flash Sector 11 NVS, connects to your router, obtains an IP via DHCP, and starts the AirPlay receiver.

---

### Method B: Pre-configured Credentials in Code
You can also hardcode your Wi-Fi router name and password directly in [src/main.c](file:///d:/BCA182_Lab/Spark_AIrplay_1/src/main.c#L34-L35):
```c
#define CONFIG_WIFI_SSID    "Your_Home_WiFi"
#define CONFIG_WIFI_PASS    "Your_WiFi_Password"
```
Re-flash via `pio run -t upload`. The board connects on boot and saves the credentials to flash.

---

### Method C: Standalone AirPlay Hotspot (Zero-Config / Offline)
If the Captive Portal times out without any submission and no router credentials exist:
1. The board automatically hosts a dedicated standalone audio hotspot:
   - **SSID**: `RT-Spark-Audio`
   - **Password**: `12345678`
   - **IP**: `192.168.4.1`
2. Connect your Apple device directly to `RT-Spark-Audio`, open Control Center, select **RT-Spark Hi-Fi**, and stream music offline with zero router required.

---

## 7. Controls & Telemetry

- **Volume Control**:
  - D-Pad UP: Volume +5%
  - D-Pad DOWN: Volume -5%
  - Volume attenuation curve also updates synchronously from Apple devices over RTSP `SET_PARAMETER`.
- **Display**:
  - Top Bar: Wi-Fi status, IP address, and volume percentage.
  - Middle: Track title and artist parsed from DMAP metadata.
  - Bottom: Real-time dual-channel (Left / Right) peak VU meters.
- **Visualizer**: 19-LED SK6805 RGB ring mirrors the audio signal dynamics.
