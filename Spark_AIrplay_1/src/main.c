/**
 * @file main.c
 * @brief Standalone AirPlay 1 (RAOP) Receiver on RT-Thread RT-Spark Board (STM32F407ZGT6)
 *        using onboard RW007 SPI Wi-Fi Module and Everest ES8388 Stereo Codec.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "rw007_wifi.h"
#include "rw007_netif.h"
#include "wifi_storage.h"
#include "wifi_portal.h"
#include "audio_codec_es8388.h"
#include "lcd_st7789.h"
#include "led_matrix.h"
#include "buttons.h"
#include "raop.h"
#include "raop_rtp.h"
#include "audio.h"
#include "mdns_responder.h"
#include "raop_crypto.h"

static K_SEM_DEFINE(s_ip_sem, 0, 1);
static struct net_mgmt_event_callback s_ip_cb;

static void ip_mgmt_event_handler(struct net_mgmt_event_callback *cb,
                                 uint64_t mgmt_event,
                                 struct net_if *iface)
{
    ARG_UNUSED(cb);
    ARG_UNUSED(iface);
    if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD || mgmt_event == NET_EVENT_IPV4_DHCP_BOUND) {
        k_sem_give(&s_ip_sem);
    }
}

/* -------------------------------------------------------------------------- */
/* Wi-Fi & Audio Configuration                                                */
/* -------------------------------------------------------------------------- */
/* Set your local Wi-Fi router SSID and password here:                        */
/* If left as "" (empty), the board starts its own SoftAP Hotspot:           */
/*   SSID: "RT-Spark-Audio"  Password: "12345678"                             */
#define CONFIG_WIFI_SSID        ""
#define CONFIG_WIFI_PASS        ""

#define DEFAULT_VOLUME          80
/* 3840 stereo frames = 87 ms of 44.1 kHz output. The ring is THE jitter
 * buffer on the F407 (no PSRAM like the ESP32 reference: it carries ~4 s);
 * feed_pcm() backpressures into it instead of dropping, so every byte of it
 * absorbs Wi-Fi bursts. 3 KB were reclaimed for this from the handshake-only
 * RSA crypto heap (raop_crypto.c) — sized against on-hardware evidence so the
 * OAEP handshake keeps ~40% heap headroom. DTCM budget unchanged. */
#define PCM_RING_FRAMES         (3840U)
#define PCM_RING_SAMPLES        (PCM_RING_FRAMES * 2U)

/* Audio Ring Buffer in DTCM RAM for 0-wait-state access */
#if defined(__arm__)
static __attribute__((section(".dtcm_noinit"), aligned(4))) int16_t s_pcm_ring[PCM_RING_SAMPLES];
#else
static int16_t s_pcm_ring[PCM_RING_SAMPLES];
#endif

static volatile uint32_t s_pcm_head = 0;
static volatile uint32_t s_pcm_tail = 0;
static struct k_spinlock s_ring_lock;
static volatile bool s_primed = false;
#define PCM_PRIME_THRESHOLD     (352U * 2U * 7U) /* ~56 ms pre-buffering before playback starts */

static volatile int16_t s_peak_l = 0;
static volatile int16_t s_peak_r = 0;
static volatile uint8_t s_volume = DEFAULT_VOLUME;
static volatile bool s_is_streaming = false;

/* Stream-health counters (telemetry: distinguish buffer starvation from CPU overload) */
static volatile uint32_t s_underruns = 0;   /* drain callback hit an empty ring */

static char s_track_title[64]  = "Ready for AirPlay";
static char s_track_artist[64] = "Connect iPhone/Mac";
static char s_track_album[64]  = "RT-Spark Hi-Fi";
static char s_ip_str[20]       = "0.0.0.0";
static bool s_wifi_connected   = false;

/* -------------------------------------------------------------------------- */
/* Audio Interface (audio.h Implementation)                                  */
/* -------------------------------------------------------------------------- */
bool audio_producer_acquire(const char *name)
{
    ARG_UNUSED(name);
    k_spinlock_key_t key = k_spin_lock(&s_ring_lock);
    s_primed = false;
    s_pcm_head = 0;
    s_pcm_tail = 0;
    memset(s_pcm_ring, 0, sizeof(s_pcm_ring));
    k_spin_unlock(&s_ring_lock, key);
    s_is_streaming = true;
    s_underruns = 0;
    printk("[Audio] Producer acquired: streaming active\n");
    return true;
}

void audio_producer_release(void)
{
    s_is_streaming = false;
    k_spinlock_key_t key = k_spin_lock(&s_ring_lock);
    s_primed = false;
    s_pcm_head = 0;
    s_pcm_tail = 0;
    memset(s_pcm_ring, 0, sizeof(s_pcm_ring));
    k_spin_unlock(&s_ring_lock, key);
    printk("[Audio] Producer released: streaming stopped\n");
}

/* Backpressured producer API (mirrors the ESP32 reference): accepts as many
 * whole stereo frames as currently fit and returns that count — NEVER drops
 * unplayed audio to make room. Callers retry until the ring drains. */
size_t audio_play_pcm(const int16_t *pcm, size_t frames)
{
    if (pcm == NULL || frames == 0) return 0;
    size_t samples = frames * 2U;

    k_spinlock_key_t key = k_spin_lock(&s_ring_lock);

    /* Calculate available free sample slots in ring buffer */
    uint32_t free_samples = (s_pcm_tail > s_pcm_head) ?
                            (s_pcm_tail - s_pcm_head - 1U) :
                            (PCM_RING_SAMPLES - s_pcm_head + s_pcm_tail - 1U);

    size_t take = samples;
    if (take > free_samples) {
        take = free_samples;
    }
    take &= ~1U; /* whole stereo frames only: keep L/R phase aligned */

    for (size_t i = 0; i < take; i++) {
        s_pcm_ring[s_pcm_head] = pcm[i];
        s_pcm_head = (s_pcm_head + 1U) % PCM_RING_SAMPLES;
    }

    uint32_t avail = (s_pcm_head >= s_pcm_tail) ?
                     (s_pcm_head - s_pcm_tail) :
                     (PCM_RING_SAMPLES - s_pcm_tail + s_pcm_head);
    if (!s_primed && avail >= PCM_PRIME_THRESHOLD) {
        s_primed = true;
    }
    k_spin_unlock(&s_ring_lock, key);

    return take / 2U;
}

void audio_airplay_set_volume(float db)
{
    /* AirPlay volume scale: -144.0 dB (mute) to 0.0 dB (max) */
    int vol = 0;
    if (db <= -144.0f) {
        vol = 0;
    } else if (db >= 0.0f) {
        vol = 100;
    } else {
        /* Standard RAOP attenuation curve: -30 dB to 0 dB maps to 0..100% */
        vol = (int)((db + 30.0f) * (100.0f / 30.0f));
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
    }
    s_volume = (uint8_t)vol;
    audio_set_volume(s_volume);
    printk("[Audio] AirPlay Volume: %.1f dB -> %u%%\n", (double)db, s_volume);
}

void audio_diag_tone_stop(void) {}
void audio_diag_tone_start(void) {}

void audio_get_stream_health(uint32_t *underruns, uint32_t *dma_misses)
{
    if (underruns)  *underruns  = s_underruns;
    if (dma_misses) *dma_misses = audio_get_diagnostics()->dma_misses;
}

/* DMA Hardware Drain Callback from ES8388 driver */
static void audio_pcm_drain_callback(int16_t *buf, size_t samples)
{
    int16_t peak_l = 0, peak_r = 0;

    k_spinlock_key_t key = k_spin_lock(&s_ring_lock);

    if (!s_primed) {
        /* Jitter buffer accumulating initial prime frames */
        memset(buf, 0, samples * sizeof(int16_t));
        k_spin_unlock(&s_ring_lock, key);
        s_peak_l = 0;
        s_peak_r = 0;
        return;
    }

    uint32_t avail = (s_pcm_head >= s_pcm_tail) ?
                     (s_pcm_head - s_pcm_tail) :
                     (PCM_RING_SAMPLES - s_pcm_tail + s_pcm_head);

    for (uint16_t i = 0; i < samples; i += 2) {
        int16_t l = 0, r = 0;
        if (avail >= 2) {
            l = s_pcm_ring[s_pcm_tail];
            s_pcm_tail = (s_pcm_tail + 1U) % PCM_RING_SAMPLES;
            r = s_pcm_ring[s_pcm_tail];
            s_pcm_tail = (s_pcm_tail + 1U) % PCM_RING_SAMPLES;
            avail -= 2;
        } else {
            /* Ring ran dry mid-block: real-time output starved by the source */
            s_underruns++;
        }

        buf[i]     = l;
        buf[i + 1] = r;

        int16_t al = (l < 0) ? -l : l;
        int16_t ar = (r < 0) ? -r : r;
        if (al > peak_l) peak_l = al;
        if (ar > peak_r) peak_r = ar;
    }
    k_spin_unlock(&s_ring_lock, key);

    s_peak_l = peak_l;
    s_peak_r = peak_r;
}

/* -------------------------------------------------------------------------- */
/* Metadata Notification Callback                                            */
/* -------------------------------------------------------------------------- */
void airplay_notify_metadata(const char *title, const char *artist, const char *album)
{
    if (title && title[0])  strncpy(s_track_title, title, sizeof(s_track_title) - 1);
    if (artist && artist[0]) strncpy(s_track_artist, artist, sizeof(s_track_artist) - 1);
    if (album && album[0])   strncpy(s_track_album, album, sizeof(s_track_album) - 1);

    printk("[Metadata] Title: '%s', Artist: '%s', Album: '%s'\n",
           s_track_title, s_track_artist, s_track_album);
}

/* -------------------------------------------------------------------------- */
/* Display UI Renderer                                                        */
/* -------------------------------------------------------------------------- */
/* The FSMC full-screen repaint costs ~6 ms; only pay it when the static UI
 * actually changes. Between changes, refresh just the two VU bars (~400 px). */
static bool     s_ui_full_repaint   = true;
static bool     s_ui_last_streaming = false;
static uint8_t  s_ui_last_volume    = 0xFF;
static char     s_ui_last_title[64]  = "";
static char     s_ui_last_artist[64] = "";

static void render_vu_bar(uint16_t y, uint16_t new_v)
{
    uint16_t color = (new_v > 170) ? LCD_COLOR_RED
                   : ((new_v > 120) ? LCD_COLOR_YELLOW : LCD_COLOR_GREEN);

    lcd_fill_rect(25, y, 225, y + 8, LCD_COLOR_BLACK); /* clear bar track */
    if (new_v > 0) {
        lcd_fill_rect(25, y, 25 + new_v, y + 8, color);
    }
}

static void render_ui(void)
{
    bool streaming = s_is_streaming;
    bool changed = s_ui_full_repaint ||
                   (streaming != s_ui_last_streaming) ||
                   (s_volume != s_ui_last_volume) ||
                   (strncmp(s_track_title, s_ui_last_title,
                            sizeof(s_track_title)) != 0) ||
                   (strncmp(s_track_artist, s_ui_last_artist,
                            sizeof(s_track_artist)) != 0);

    if (changed) {
        char line[40];

        /* Top Status Bar: Background Navy */
        lcd_fill_rect(0, 0, LCD_WIDTH - 1, 24, LCD_COLOR_NAVY);
        snprintf(line, sizeof(line), "IP:%s  V:%u%%", s_ip_str, s_volume);
        lcd_show_string(6, 4, line, LCD_COLOR_CYAN, LCD_COLOR_NAVY);

        /* Main Area: Background Black */
        lcd_fill_rect(0, 25, LCD_WIDTH - 1, 185, LCD_COLOR_BLACK);

        /* Stream State Header */
        if (streaming) {
            lcd_show_string(20, 36, "[AirPlay Streaming]", LCD_COLOR_GREEN, LCD_COLOR_BLACK);
        } else {
            lcd_show_string(20, 36, "[AirPlay 1 Receiver]", LCD_COLOR_YELLOW, LCD_COLOR_BLACK);
        }

        /* Track Metadata */
        lcd_show_string(10, 65, "Track:", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
        lcd_show_string(10, 85, s_track_title, LCD_COLOR_WHITE, LCD_COLOR_BLACK);

        lcd_show_string(10, 115, "Artist:", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
        lcd_show_string(10, 135, s_track_artist, LCD_COLOR_WHITE, LCD_COLOR_BLACK);

        lcd_show_string(10, 160, "Format: ALAC 44.1k Stereo", LCD_COLOR_DARKCYAN, LCD_COLOR_BLACK);

        /* VU Labels */
        lcd_fill_rect(0, 186, LCD_WIDTH - 1, LCD_HEIGHT - 1, LCD_COLOR_BLACK);
        lcd_show_string(6, 195, "L:", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
        lcd_show_string(6, 215, "R:", LCD_COLOR_WHITE, LCD_COLOR_BLACK);

        snprintf(s_ui_last_title, sizeof(s_ui_last_title), "%s", s_track_title);
        snprintf(s_ui_last_artist, sizeof(s_ui_last_artist), "%s", s_track_artist);
        s_ui_last_volume    = s_volume;
        s_ui_last_streaming = streaming;
        s_ui_full_repaint   = false;
    }

    /* VU Peak Level Meters (bottom, y=190..235): cheap partial refresh */
    uint16_t vu_l = (uint16_t)(((uint32_t)s_peak_l * 200U) / 32768U);
    uint16_t vu_r = (uint16_t)(((uint32_t)s_peak_r * 200U) / 32768U);
    if (vu_l > 200) vu_l = 200;
    if (vu_r > 200) vu_r = 200;
    render_vu_bar(198, vu_l);
    render_vu_bar(218, vu_r);
}

/* -------------------------------------------------------------------------- */
/* LED Matrix Visualizer Task                                                 */
/* -------------------------------------------------------------------------- */
static void update_led_visualizer(void)
{
    /* led_matrix_show() locks IRQs for ~570 us of bit-banged WS2812 timing;
     * skip the transfer entirely when the frame would be identical. */
    static bool last_streaming = false;
    static uint8_t last_level = 0xFF;
    static bool last_clip = false;

    bool clip = (s_peak_l > 20000) || (s_peak_r > 20000);
    uint8_t level = (uint8_t)(((uint32_t)(s_peak_l + s_peak_r) / 2U * 16U) / 32768U);

    if (s_is_streaming == last_streaming && level == last_level && clip == last_clip) {
        return;
    }
    last_streaming = s_is_streaming;
    last_level = level;
    last_clip = clip;

    if (!s_is_streaming) {
        /* Idle breathing glow on center LED */
        led_matrix_clear();
        led_matrix_set_pixel(16, 0, 10, 30);
        led_matrix_show();
        return;
    }

    /* Active music visualization: map peak levels across 19 LEDs */
    for (uint8_t i = 0; i < 16; i++) {
        if (i <= level) {
            uint8_t r = (i > 11) ? 40 : 0;
            uint8_t g = (i > 6 && i <= 11) ? 35 : 0;
            uint8_t b = (i <= 6) ? 30 : 0;
            led_matrix_set_pixel(i, r, g, b);
        } else {
            led_matrix_set_pixel(i, 0, 0, 0);
        }
    }
    led_matrix_set_pixel(17, (s_peak_l > 20000) ? 50 : 0, 30, 0);
    led_matrix_set_pixel(18, (s_peak_r > 20000) ? 50 : 0, 30, 0);
    led_matrix_show();
}

/* -------------------------------------------------------------------------- */
/* Network Event Handler (Zephyr DHCP Event)                                 */
/* -------------------------------------------------------------------------- */
static void start_airplay_services(uint32_t ip_addr)
{
    uint8_t mac[6];
    rw007_wifi_get_mac(mac);

    char instance_name[64];
    snprintf(instance_name, sizeof(instance_name), "%02X%02X%02X%02X%02X%02X@RT-Spark Hi-Fi",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* Start mDNS service */
    mdns_responder_start("rtspark", instance_name, RAOP_RTSP_PORT, ip_addr);

    /* Start RTSP Server */
    raop_server_start();

    printk("\n=======================================================\n");
    printk(" AirPlay 1 (RAOP) Receiver Online!\n");
    printk(" Discovered as: '%s'\n", instance_name);
    printk(" IP Address:    %s\n", s_ip_str);
    printk(" Audio Output:  Everest ES8388 Codec (3.5mm Jack @ 44.1k)\n");
    printk("=======================================================\n");
}

/* -------------------------------------------------------------------------- */
/* Stall watchdog                                                             */
/* -------------------------------------------------------------------------- */
/* Observed failure: mid-stream the RW007 stops delivering unicast frames (the
 * SPI link stays healthy, no DISCONNECT event, RSSI queries still answer) and
 * never recovers by itself. Silence on the audio socket while a session is live
 * and the sender has not paused is treated as that stall and answered by leaving
 * and rejoining the access point. This is a workaround for the symptom, not a
 * fix for its cause (unknown; the chip firmware is closed). */
#define STALL_REJOIN_MS       5000U    /* silent this long while RECORDING => stalled */
#define STALL_RETRY_GAP_MS    15000U   /* minimum spacing between rejoin attempts */
#define STALL_MAX_REJOINS     3        /* give up after this many in a row */

static bool wifi_rejoin(const char *ssid, const char *pass)
{
    printk("[Wi-Fi] WATCHDOG: audio stalled, rejoining '%s'\n", ssid);
    struct net_if *iface = rw007_netif_get();

    rw007_wifi_disconnect();
    k_msleep(500);
    if (!rw007_wifi_connect(ssid, pass)) {
        printk("[Wi-Fi] WATCHDOG: rejoin failed\n");
        return false;
    }

    k_sem_reset(&s_ip_sem);
    net_dhcpv4_restart(iface);
    bool leased = (k_sem_take(&s_ip_sem, K_SECONDS(10)) == 0);
    printk("[Wi-Fi] WATCHDOG: rejoined, DHCP %s\n", leased ? "bound" : "no event (link may still be up)");
    return true;
}

/* -------------------------------------------------------------------------- */
/* Main Application Entry Point                                               */
/* -------------------------------------------------------------------------- */
int main(void)
{
    /* Drop the UI thread below every real-time producer. Zephyr's default
     * main priority (0) would preempt the RTP receive thread (4), the RW007
     * RX thread (3) and the audio producer (1) for ~6-8 ms every render tick,
     * building a packet backlog that starves the jitter buffer and stutters
     * the audio stream. */
    k_thread_priority_set(k_current_get(), 10);

    printk("\n==================================================\n");
    printk(" RT-Thread RT-Spark AirPlay 1 Receiver (Zephyr RTOS)\n");
    printk(" Hardware: STM32F407ZGT6 + RW007 Wi-Fi + ES8388\n");
    printk("==================================================\n");

    /* 1. Initialize LCD Display */
    lcd_st7789_init();
    lcd_clear(LCD_COLOR_BLACK);
    lcd_show_string(20, 100, "Starting AirPlay 1...", LCD_COLOR_YELLOW, LCD_COLOR_BLACK);

    /* 2. Initialize ES8388 Audio Codec */
    audio_hardware_init();
    audio_set_volume(s_volume);
    audio_set_pcm_callback(audio_pcm_drain_callback);

    /* 3. Initialize RAOP Crypto (2048-bit RSA Private Key) on boot stack */
    printk("[Init] Initializing RAOP Crypto Engine...\n");
    if (raop_crypto_init() != ESP_OK) {
        printk("[Init] ERROR: RAOP RSA key initialization failed!\n");
    }

    /* 4. Initialize SK6805 LED Matrix & Buttons */
    led_matrix_init();
    led_matrix_clear();
    buttons_init();

    /* 4. Initialize Wi-Fi Storage */
    char ssid[WIFI_SSID_MAX_LEN] = {0};
    char pass[WIFI_PASS_MAX_LEN] = {0};
    bool has_creds = wifi_storage_read(ssid, pass);
    if ((!has_creds || ssid[0] == '\0') && strlen(CONFIG_WIFI_SSID) > 0) {
        strncpy(ssid, CONFIG_WIFI_SSID, sizeof(ssid) - 1);
        strncpy(pass, CONFIG_WIFI_PASS, sizeof(pass) - 1);
        has_creds = true;
    }
    /* 5. Initialize RW007 Wi-Fi Module */
    printk("[Init] Initializing RW007 Wi-Fi Module...\n");
    if (!rw007_wifi_init()) {
        printk("[Init] ERROR: RW007 hardware initialization failed!\n");
        lcd_show_string(20, 120, "RW007 Init Error!", LCD_COLOR_RED, LCD_COLOR_BLACK);
    } else {
        printk("[Init] RW007 Wi-Fi Firmware: %s\n", rw007_wifi_get_version());
    }

    /* Check if user holds D-Pad CENTER button on boot to force captive portal */
    bool force_portal = (buttons_poll() == BTN_CENTER);

    /* 6. Launch Captive Portal if no credentials exist or forced by user */
    if (!has_creds || force_portal) {
        printk("[Wi-Fi] No credentials found or portal forced. Starting Captive Portal...\n");
        if (wifi_portal_run(120, ssid, pass)) {
            has_creds = true;
        }
    }

    /* 7. Connect to Wi-Fi */
    net_mgmt_init_event_callback(&s_ip_cb, ip_mgmt_event_handler,
                                 NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IPV4_DHCP_BOUND);
    net_mgmt_add_event_callback(&s_ip_cb);

    if (has_creds && ssid[0] != '\0') {
        printk("[Wi-Fi] Connecting to Wi-Fi SSID '%s'...\n", ssid);
        lcd_clear(LCD_COLOR_BLACK);
        lcd_show_string(20, 100, "Connecting Wi-Fi...", LCD_COLOR_CYAN, LCD_COLOR_BLACK);
        lcd_show_string(20, 125, ssid, LCD_COLOR_WHITE, LCD_COLOR_BLACK);

        uint8_t sta_mac[6];
        rw007_wifi_get_mac(sta_mac);
        rw007_netif_set_mac(sta_mac);

        if (rw007_wifi_connect(ssid, pass)) {
            printk("[Wi-Fi] Associated with '%s'! Requesting DHCP lease...\n", ssid);
            lcd_show_string(20, 150, "Acquiring IP...", LCD_COLOR_YELLOW, LCD_COLOR_BLACK);

            k_sem_reset(&s_ip_sem);
            rw007_netif_link(true);
            struct net_if *iface = rw007_netif_get();
            net_dhcpv4_start(iface);

            bool leased = false;
            for (int i = 0; i < 30 && !leased; i++) {
                leased = (k_sem_take(&s_ip_sem, K_SECONDS(1)) == 0);
            }

            if (leased) {
                s_wifi_connected = true;
                wifi_storage_save(ssid, pass);
                printk("[Wi-Fi] DHCP lease acquired!\n");

                /* Streaming needs the radio awake: report, disable, and verify
                 * the module's power-save mode (encoding undocumented, so the
                 * raw readback is logged to confirm what actually took effect). */
                int32_t ps_before = -1, ps_after = -1, rssi = 0;
                rw007_wifi_power_save_get(&ps_before);
                rw007_wifi_power_save_set(0);
                rw007_wifi_power_save_get(&ps_after);
                if (rw007_wifi_get_rssi(&rssi) == 0) {
                    printk("[Wi-Fi] power_save %d -> %d, RSSI %d dBm\n",
                           (int)ps_before, (int)ps_after, (int)rssi);
                } else {
                    printk("[Wi-Fi] power_save %d -> %d, RSSI unavailable\n",
                           (int)ps_before, (int)ps_after);
                }
            } else {
                printk("[Wi-Fi] DHCP lease timed out!\n");
                net_dhcpv4_stop(iface);
                rw007_netif_link(false);
            }
        } else {
            printk("[Wi-Fi] Failed to connect to '%s'!\n", ssid);
            lcd_show_string(20, 150, "Connection Failed!", LCD_COLOR_RED, LCD_COLOR_BLACK);
            k_msleep(1500);
        }
    }

    /* 8. Fallback to Standalone AirPlay Hotspot if not connected */
    if (!s_wifi_connected) {
        printk("[Wi-Fi] Starting Standalone AirPlay Hotspot 'RT-Spark-Audio'...\n");
        lcd_clear(LCD_COLOR_BLACK);
        lcd_show_string(20, 100, "Hotspot Active:", LCD_COLOR_YELLOW, LCD_COLOR_BLACK);
        lcd_show_string(20, 125, "RT-Spark-Audio", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
        rw007_wifi_start_softap("RT-Spark-Audio", "12345678");
        strncpy(s_ip_str, "192.168.4.1", sizeof(s_ip_str) - 1);
        uint32_t ap_ip = 0x0104A8C0;
        start_airplay_services(ap_ip);
    } else {
        struct net_if *iface = rw007_netif_get();
        struct net_if_ipv4 *ipv4 = iface ? iface->config.ip.ipv4 : NULL;
        uint32_t sta_ip = 0;
        if (ipv4) {
            for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
                if (ipv4->unicast[i].ipv4.is_used) {
                    struct in_addr *addr = &ipv4->unicast[i].ipv4.address.in_addr;
                    sta_ip = addr->s_addr;
                    uint8_t *b = (uint8_t *)&addr->s_addr;
                    snprintf(s_ip_str, sizeof(s_ip_str), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
                    break;
                }
            }
        }
        printk("[Wi-Fi] Station Online! IP: %s\n", s_ip_str);
        start_airplay_services(sta_ip);
    }

    /* 7. Super Loop: UI rendering, LED visualizer, Button polling */
    uint32_t last_ui_tick = 0;
    uint32_t last_rssi_tick = 0;
    uint32_t last_rejoin_tick = 0;
    uint32_t rejoins_in_row = 0;
    while (1) {
        uint32_t now = k_uptime_get_32();

        /* Stall watchdog (station mode only; the SoftAP fallback has nothing to rejoin) */
        if (s_wifi_connected && s_is_streaming) {
            uint32_t stalled = raop_rtp_stalled_ms();
            if (stalled < 1000U) {
                rejoins_in_row = 0;                       /* audio is flowing again */
            } else if (stalled >= STALL_REJOIN_MS &&
                       rejoins_in_row < STALL_MAX_REJOINS &&
                       (last_rejoin_tick == 0 || (now - last_rejoin_tick) >= STALL_RETRY_GAP_MS)) {
                rejoins_in_row++;
                printk("[Wi-Fi] WATCHDOG: no audio for %u ms (attempt %u/%u)\n",
                       (unsigned)stalled, (unsigned)rejoins_in_row, (unsigned)STALL_MAX_REJOINS);
                (void)wifi_rejoin(ssid, pass);
                last_rejoin_tick = k_uptime_get_32();
            }
        }

        /* Poll navigation buttons */
        button_id_t btn = buttons_poll();
        if (btn == BTN_UP) {
            if (s_volume <= 95) s_volume += 5;
            else s_volume = 100;
            audio_set_volume(s_volume);
            printk("[Button] Volume UP -> %u%%\n", s_volume);
        } else if (btn == BTN_DOWN) {
            if (s_volume >= 5) s_volume -= 5;
            else s_volume = 0;
            audio_set_volume(s_volume);
            printk("[Button] Volume DOWN -> %u%%\n", s_volume);
        }

        /* Link-quality sample while streaming. Runs on this low-priority thread
         * on purpose: the query waits on the module and can block for seconds
         * when the link stalls, which must never happen on the audio path. */
        if (s_is_streaming && (now - last_rssi_tick) >= 2000) {
            last_rssi_tick = now;
            int32_t rssi = 0;
            if (rw007_wifi_get_rssi(&rssi) == 0) {
                printk("[Wi-Fi] RSSI %d dBm\n", (int)rssi);
            } else {
                printk("[Wi-Fi] RSSI query failed\n");
            }
        }

        /* Periodic UI & LED update */
        if (now - last_ui_tick >= 60) {
            last_ui_tick = now;
            render_ui();
            update_led_visualizer();
        }

        k_msleep(10);
    }

    return 0;
}
