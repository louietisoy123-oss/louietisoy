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
| **RW007 Wi-Fi** | SPI2 SCK | `PB13` | SPI Clock (10.5 MHz, mode 0) |
| | SPI2 MISO | `PC2` | Master In / Slave Out |
| | SPI2 MOSI | `PC3` | Master Out / Slave In |
| | RW007 CS | `PF10` | Slave Select (Active Low) |
| | RW007 RST | `PG15` | Hardware Reset |
| | RW007 INT/BUSY | `PG11` | Falling edge = module needs service |
| **ES8388 Audio** | I2S3 WS (LRCK) | `PA15` | Word Select (Left/Right Clock) |
| | I2S3 CK (BCLK) | `PB3` | Bit Clock |
| | I2S3 SD (DIN) | `PB5` | Serial Data Out (to ES8388 DAC) |
| | I2S3 MCK | `PC7` | Master Clock |
| | I2C SCL / SDA | `PF1 / PF0` | Bit-banged codec control (address 0x10) |
| **ST7789 LCD** | FSMC D0..D7 | `PD14, PD15, PD0, PD1, PE7..PE10` | FSMC Bank 1 NE3, 8-bit 8080 bus |
| | DCX / RD / WR | `PD13 (A18) / PD4 / PD5` | Data-command select / read / write strobes |
| | RST / BL | `PD3 / PF9` | Reset / Backlight |
| **RGB LED** | SK6805 DIN | `PF2` | 19-LED RGB visualizer data |
| **Buttons** | D-Pad | `PC5 up, PC1 down, PC0 left, PC4 right, PA0 centre` | Active low |

---

## 4. Software Stack & Memory Architecture

- **Operating System**: Zephyr RTOS v4.4.0 (L2 Ethernet, BSD Sockets, IGMP, Multicast DNS).
- **Crypto Suite**: Vendored standalone mbedTLS 3.6 cryptographic primitives (`aes`, `rsa`, `bignum`, `sha1`, `md`, `asn1parse`, `pkparse`) compiled directly into `src/crypto/` to eliminate TF-PSA-Crypto conflicts.
- **DTCM Placement (64 KB @ 0x10000000)**:
  - 16-bit stereo PCM audio ring buffer (`s_pcm_ring`, 3840 frames = ~87 ms) — the streaming jitter buffer; the RTP path backpressures into it (retry-on-full, never drops mid-stream).
  - RW007 SPI high-speed frame transfer buffers (`s_tx_frame`, `s_rx_frame`).
  - mbedTLS crypto heap (11 KB, used only during the RTSP handshake: RSA key parse / challenge sign / OAEP decrypt — sized against on-hardware evidence; 8 KB OOM'd during OAEP).
  - ALAC decode state buffers.
- **Internal SRAM (128 KB)**:
  - Zephyr kernel threads and networking buffers (22 RX packets / 42 RX net_bufs ≈ 160 ms of burst headroom).
  - RTP reorder window lives in DTCM: 8 packets (~64 ms) with 4-packet (32 ms) hold before silence concealment.

---

## 4b. Testing the Receiver (Python test sender)

`tools/send_test_audio.py` is a full AirPlay 1 sender-side test rig (Python 3, `pip install cryptography pyserial`):

```bash
cd Spark_AIrplay_1/tools

python send_test_audio.py                      # 10 s chord-arp tone, auto-discover the board
python send_test_audio.py --pc                 # stream this PC's live audio output (WASAPI loopback)
python send_test_audio.py --wave ../../tmp/lukaluka_44100.wav --duration 40
python send_test_audio.py --ip 192.168.1.56 --serial COM7 --volume-db -10
python send_test_audio.py --list-ports         # COM ports | --list-pc-devices: loopback devices
```

- **Discovery order**: `--ip` > SoftAP heuristic (192.168.4.x) > ARP cache > one-shot mDNS (`_raop._tcp`) > serial boot log > concurrent /24 scan. The verified RTSP connection is adopted, not re-opened (the server is single-client).
- **WAV mode** requires 44.1 kHz / 16-bit / stereo: `ffmpeg -i in.wav -ar 44100 -ac 2 -sample_fmt s16 out.wav`.
- **`--pc` mode** captures whatever the PC is playing (WASAPI loopback, `pip install soundcard numpy`), rate-converts it to 44.1 kHz, and pads the send timeline with silence to the wall clock — so the board's jitter buffer never runs dry; content gaps play as silence.
- The script services the board's 0xD5 resend requests and answers 0xD2 timing requests, paces RTP at real time with sub-millisecond drift, and prints the board's health telemetry (`[RT-Spark] health: underruns/ring_stalls/conceals/resends/dma_misses/decode_*`).

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

---

## 8. Stutter Investigation & Known Limitation

Playback works, but long streams still drop out. These are the findings from the stutter investigation.

**Ruled out on hardware (board side is not the bottleneck):**
- **CPU / RAM:** with the board's own test sender, the audio pipeline runs with `underruns=0` and `dma_misses=0` whenever packets actually arrive. Decode and I2S keep up easily.
- **Rendering:** the LCD was blocking the audio thread. The UI thread now runs at priority 10, redraws only what changed, and skips LED frames while audio is under pressure.
- **Buffering:** the jitter path was rebuilt. The RTP path now backpressures into the ring instead of dropping. The PCM ring holds 3840 frames (~87 ms), 7 packets (~56 ms) are buffered before playback starts, and the reorder window is 8 packets with a 4-packet hold. Net RX buffers went to 22 packets / 42 bufs.
- **Wi-Fi power save:** the RW007 power-save mode is read, disabled and read back after DHCP. It already reports 0.
- **Signal / router:** RSSI is sampled and logged, and the drops also happen on a laptop hotspot one metre away.

**Root cause:** after about 10–35 s of continuous streaming, the **RW007 stops delivering unicast frames** to the STM32. There is no DISCONNECT event, the SPI handshake stays healthy, and the RSSI is fine. The stall is inside the RW007 module firmware, which is a closed binary.

**Mitigation in firmware:** a stall watchdog in `main.c` rejoins the network if no audio arrives for 5 s while recording (`STALL_REJOIN_MS`), at most 3 times in a row, at least 15 s apart. It has only been exercised with a simulated stall so far.

The USB path doesn't depend on the RW007. For glitch-free PC playback on this board, use [`../Spark_USB_DAC`](../Spark_USB_DAC).
