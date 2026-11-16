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
#define PCM_RING_FRAMES         (3072U)
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
#define PCM_PRIME_THRESHOLD     (352U * 2U * 4U) /* ~4 packets (32 ms) pre-buffering */

static volatile int16_t s_peak_l = 0;
static volatile int16_t s_peak_r = 0;
static volatile uint8_t s_volume = DEFAULT_VOLUME;
static volatile bool s_is_streaming = false;

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

size_t audio_play_pcm(const int16_t *pcm, size_t frames)
{
    if (pcm == NULL || frames == 0) return 0;
    size_t samples = frames * 2U;

    k_spinlock_key_t key = k_spin_lock(&s_ring_lock);

    /* Calculate available free sample slots in ring buffer */
    uint32_t free_samples = (s_pcm_tail > s_pcm_head) ?
                            (s_pcm_tail - s_pcm_head - 1U) :
                            (PCM_RING_SAMPLES - s_pcm_head + s_pcm_tail - 1U);

    if (samples > free_samples) {
        /* Discard only the oldest stereo frames necessary to fit new data */
        uint32_t discard = samples - free_samples;
        discard = (discard + 1U) & ~1U; /* Keep stereo L/R phase aligned */
        s_pcm_tail = (s_pcm_tail + discard) % PCM_RING_SAMPLES;
    }

    for (size_t i = 0; i < samples; i++) {
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

    return frames;
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
static void render_ui(void)
{
    char line[40];

    /* Top Status Bar: Background Navy */
    lcd_fill_rect(0, 0, LCD_WIDTH - 1, 24, LCD_COLOR_NAVY);
    snprintf(line, sizeof(line), "IP:%s  V:%u%%", s_ip_str, s_volume);
    lcd_show_string(6, 4, line, LCD_COLOR_CYAN, LCD_COLOR_NAVY);

    /* Main Area: Background Black */
    lcd_fill_rect(0, 25, LCD_WIDTH - 1, 185, LCD_COLOR_BLACK);

    /* Stream State Header */
    if (s_is_streaming) {
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

    /* VU Peak Level Meters at Bottom (y=190..235) */
    uint16_t vu_l = (uint16_t)(((uint32_t)s_peak_l * 200U) / 32768U);
    uint16_t vu_r = (uint16_t)(((uint32_t)s_peak_r * 200U) / 32768U);
    if (vu_l > 200) vu_l = 200;
    if (vu_r > 200) vu_r = 200;

    lcd_fill_rect(0, 186, LCD_WIDTH - 1, LCD_HEIGHT - 1, LCD_COLOR_BLACK);
    lcd_show_string(6, 195, "L:", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    lcd_fill_rect(25, 198, 25 + vu_l, 206, (vu_l > 170) ? LCD_COLOR_RED : ((vu_l > 120) ? LCD_COLOR_YELLOW : LCD_COLOR_GREEN));

    lcd_show_string(6, 215, "R:", LCD_COLOR_WHITE, LCD_COLOR_BLACK);
    lcd_fill_rect(25, 218, 25 + vu_r, 226, (vu_r > 170) ? LCD_COLOR_RED : ((vu_r > 120) ? LCD_COLOR_YELLOW : LCD_COLOR_GREEN));
}

/* -------------------------------------------------------------------------- */
/* LED Matrix Visualizer Task                                                 */
/* -------------------------------------------------------------------------- */
static void update_led_visualizer(void)
{
    if (!s_is_streaming) {
        /* Idle breathing glow on center LED */
        led_matrix_clear();
        led_matrix_set_pixel(16, 0, 10, 30);
        led_matrix_show();
        return;
    }

    /* Active music visualization: map peak levels across 19 LEDs */
    uint8_t level = (uint8_t)(((uint32_t)(s_peak_l + s_peak_r) / 2U * 16U) / 32768U);
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
/* Main Application Entry Point                                               */
/* -------------------------------------------------------------------------- */
int main(void)
{
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
    while (1) {
        uint32_t now = k_uptime_get_32();

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
