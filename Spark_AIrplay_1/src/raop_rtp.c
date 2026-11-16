// Phase 4 RTP audio receive path for Zephyr RTOS on RT-Spark (STM32F407)
#include "raop_rtp.h"
#include "rtp_parser.h"
#include "aes_frame.h"
#include "rtp_reorder.h"
#include "rtp_resend.h"
#include "rtp_timing.h"
#include "alac.h"
#include "alac_config.h"
#include "audio.h"

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/printk.h>
#include "esp_compat.h"

#include "mbedtls/aes.h"

#include <string.h>
#include <errno.h>

static const char *TAG = "raop_rtp";

#define RTP_RX_CAP       2048
#define PCM_MAX_INT16    (1024 * 2)   /* 1024 stereo frames max */
#define REORDER_WINDOW   4            /* 4 packets @ 352 samples ≈ 32 ms buffer */
#define REORDER_HOLD     2            /* conceal after holding 2 packets (16 ms) */
#define RESEND_MIN_INTERVAL_MS 20
#define TIMING_INTERVAL_MS     3000

static alac_file            *s_alac        = NULL;
static mbedtls_aes_context   s_aes;
static bool                  s_aes_key_ok  = false;
static uint8_t               s_iv[16];
static uint8_t               s_num_channels = 2;
static uint16_t              s_frame_length = 352;

static int                   s_audio_fd    = -1;
static int                   s_control_fd  = -1;
static int                   s_timing_fd   = -1;
static int                   s_client_control_port = 0;
static int                   s_client_timing_port  = 0;

static struct sockaddr_in    s_peer;
static bool                  s_peer_known  = false;

static rtp_reorder_t         s_reorder;
static __attribute__((section(".dtcm_noinit"), aligned(4))) rtp_reorder_slot_t s_slots[REORDER_WINDOW];

static uint16_t              s_resend_first = 0;
static bool                  s_resend_valid = false;
static uint32_t              s_resend_tick  = 0;

K_THREAD_STACK_DEFINE(s_rtp_stack, 4096);
static struct k_thread       s_rtp_thread;
static volatile bool         s_run         = false;
static volatile bool         s_done        = false;

static __attribute__((section(".dtcm_noinit"), aligned(4))) uint8_t  s_rx[RTP_RX_CAP];
static __attribute__((section(".dtcm_noinit"), aligned(4))) uint8_t  s_enc[RTP_REORDER_MAX_PKT];
static __attribute__((section(".dtcm_noinit"), aligned(4))) uint8_t  s_plain[RTP_RX_CAP];
static __attribute__((section(".dtcm_noinit"), aligned(4))) int16_t  s_pcm[PCM_MAX_INT16];

static int aes_cbc_decrypt(const uint8_t *in, size_t len, const uint8_t iv[16],
                           uint8_t *out, size_t out_cap) {
    if (len > out_cap || !s_aes_key_ok) {
        return -1;
    }
    uint8_t iv_copy[16];
    memcpy(iv_copy, iv, 16);
    int rc = mbedtls_aes_crypt_cbc(&s_aes, MBEDTLS_AES_DECRYPT, len, iv_copy, in, out);
    if (rc != 0) {
        ESP_LOGW(TAG, "mbedtls_aes_crypt_cbc failed: %d", rc);
        return -1;
    }
    return 0;
}

static void decode_and_feed(const uint8_t *payload, size_t payload_len) {
    if (payload == NULL || payload_len == 0 || s_alac == NULL) return;

    aes_frame_split_t sp;
    aes_frame_split(payload_len, &sp);

    if (sp.cipher_len > 0) {
        if (aes_cbc_decrypt(payload, sp.cipher_len, s_iv,
                            s_plain, sizeof(s_plain)) != 0) {
            return;
        }
    }
    if (sp.plain_len > 0) {
        if (sp.plain_off + sp.plain_len <= sizeof(s_plain)) {
            memcpy(s_plain + sp.plain_off, payload + sp.plain_off, sp.plain_len);
        } else {
            return;
        }
    }

    int out_bytes = sizeof(s_pcm);
    alac_decode_frame(s_alac, (unsigned char *)s_plain, s_pcm, &out_bytes);
    if (out_bytes <= 0) return;

    size_t samples = (size_t)out_bytes / sizeof(int16_t);
    size_t frames  = samples / (size_t)s_num_channels;
    audio_play_pcm(s_pcm, frames);
}

static void feed_silence(size_t frame_count) {
    size_t samples = frame_count * (size_t)s_num_channels;
    if (samples > PCM_MAX_INT16) samples = PCM_MAX_INT16;
    memset(s_pcm, 0, samples * sizeof(int16_t));
    audio_play_pcm(s_pcm, samples / (size_t)s_num_channels);
}

static void send_resend_request(uint16_t first_missing, uint16_t count) {
    if (!s_peer_known || s_client_control_port == 0 || s_control_fd < 0) return;

    uint32_t now = k_uptime_get_32();
    if (s_resend_valid && s_resend_first == first_missing &&
        (now - s_resend_tick) < RESEND_MIN_INTERVAL_MS) {
        return;
    }

    uint8_t buf[RESEND_REQ_LEN];
    rtp_resend_build(buf, first_missing, count);

    struct sockaddr_in to = s_peer;
    to.sin_port = htons((uint16_t)s_client_control_port);
    sendto(s_control_fd, buf, sizeof(buf), 0, (struct sockaddr *)&to, sizeof(to));

    s_resend_first = first_missing;
    s_resend_valid = true;
    s_resend_tick  = now;
}

static void send_timing_request(void) {
    if (!s_peer_known || s_client_timing_port == 0 || s_timing_fd < 0) return;

    uint8_t buf[TIMING_PKT_LEN];
    uint64_t now_ntp = ((uint64_t)k_uptime_get_32()) << 32;
    rtp_timing_build_request(buf, now_ntp);

    struct sockaddr_in to = s_peer;
    to.sin_port = htons((uint16_t)s_client_timing_port);
    sendto(s_timing_fd, buf, sizeof(buf), 0, (struct sockaddr *)&to, sizeof(to));
}

static void drain_reorder(void) {
    for (;;) {
        size_t pop_len = 0;
        uint16_t pop_seq = 0;
        rtp_pop_t p = rtp_reorder_pop(&s_reorder, s_enc, sizeof(s_enc), &pop_len, &pop_seq);
        if (p == RTP_POP_OK) {
            decode_and_feed(s_enc, pop_len);
            s_resend_valid = false;
        } else if (p == RTP_POP_WAIT) {
            uint16_t first = 0, count = 0;
            if (rtp_reorder_gap(&s_reorder, &first, &count)) {
                send_resend_request(first, count);
            }
            break;
        } else if (p == RTP_POP_CONCEAL) {
            feed_silence(s_frame_length);
            s_resend_valid = false;
        } else {
            break;
        }
    }
}

static void handle_audio_packet(const uint8_t *pkt, size_t len) {
    rtp_header_t h;
    if (rtp_parse(pkt, len, &h) != 0) return;
    if ((h.payload_type & 0x7f) != 0x60) return;

    rtp_reorder_insert(&s_reorder, h.seq, h.payload, h.payload_len);
    drain_reorder();
}

static void handle_control_packet(const uint8_t *pkt, size_t len) {
    const uint8_t *inner = NULL;
    size_t inner_len = 0;
    if (rtp_resend_unwrap(pkt, len, &inner, &inner_len) == 0) {
        rtp_header_t h;
        if (rtp_parse(inner, inner_len, &h) == 0 &&
            (h.payload_type & 0x7f) == 0x60) {
            rtp_reorder_insert(&s_reorder, h.seq, h.payload, h.payload_len);
            drain_reorder();
        }
    }
}

static void handle_timing_packet(const uint8_t *pkt, size_t len) {
    uint8_t t_type = 0;
    timing_stamps_t st;
    if (rtp_timing_parse(pkt, len, &t_type, &st) != 0) return;

    if (t_type == TIMING_REQ_TYPE && s_peer_known && s_client_timing_port > 0) {
        uint8_t resp[TIMING_PKT_LEN];
        uint64_t now_ntp = ((uint64_t)k_uptime_get_32()) << 32;
        rtp_timing_build_response(resp, st.transmit, now_ntp, now_ntp);
        struct sockaddr_in to = s_peer;
        to.sin_port = htons((uint16_t)s_client_timing_port);
        sendto(s_timing_fd, resp, sizeof(resp), 0, (struct sockaddr *)&to, sizeof(to));
    }
}

static void rtp_task(void *a1, void *a2, void *a3) {
    ARG_UNUSED(a1); ARG_UNUSED(a2); ARG_UNUSED(a3);

    uint32_t last_timing = k_uptime_get_32();

    while (s_run) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;

        int fds[3] = { s_audio_fd, s_control_fd, s_timing_fd };
        for (int i = 0; i < 3; i++) {
            if (fds[i] >= 0) {
                FD_SET(fds[i], &rfds);
                if (fds[i] > maxfd) maxfd = fds[i];
            }
        }
        if (maxfd < 0) break;

        struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 };
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);

        if (r > 0) {
            if (s_audio_fd >= 0 && FD_ISSET(s_audio_fd, &rfds)) {
                struct sockaddr_in from;
                socklen_t flen = sizeof(from);
                int n = recvfrom(s_audio_fd, s_rx, sizeof(s_rx), 0,
                                 (struct sockaddr *)&from, &flen);
                if (n > 0) {
                    if (!s_peer_known) {
                        s_peer = from;
                        s_peer_known = true;
                    }
                    handle_audio_packet(s_rx, (size_t)n);
                }
            }
            if (s_control_fd >= 0 && FD_ISSET(s_control_fd, &rfds)) {
                int n = recvfrom(s_control_fd, s_rx, sizeof(s_rx), 0, NULL, NULL);
                if (n > 0) handle_control_packet(s_rx, (size_t)n);
            }
            if (s_timing_fd >= 0 && FD_ISSET(s_timing_fd, &rfds)) {
                int n = recvfrom(s_timing_fd, s_rx, sizeof(s_rx), 0, NULL, NULL);
                if (n > 0) handle_timing_packet(s_rx, (size_t)n);
            }
        }

        uint32_t now = k_uptime_get_32();
        if ((now - last_timing) >= TIMING_INTERVAL_MS) {
            last_timing = now;
            send_timing_request();
        }
    }

    s_done = true;
}

static int import_aes_key(const uint8_t key[16]) {
    mbedtls_aes_init(&s_aes);
    int rc = mbedtls_aes_setkey_dec(&s_aes, key, 128);
    if (rc != 0) {
        ESP_LOGE(TAG, "mbedtls_aes_setkey_dec failed: %d", rc);
        return -1;
    }
    s_aes_key_ok = true;
    return 0;
}

static void teardown_crypto_decoder(void) {
    if (s_aes_key_ok) {
        mbedtls_aes_free(&s_aes);
        s_aes_key_ok = false;
    }
    if (s_alac != NULL) {
        alac_free(s_alac);
        s_alac = NULL;
    }
}

int raop_rtp_start(raop_session_t *s) {
    if (s == NULL || s->audio_fd < 0) return -1;
    if (s_run) raop_rtp_stop();

    alac_cfg_t c;
    if (alac_cfg_from_fmtp(s->fmtp, &c) != 0) {
        ESP_LOGE(TAG, "alac_cfg_from_fmtp failed (fmtp=\"%s\")", s->fmtp);
        return -1;
    }

    s_alac = alac_create(c.bit_depth, c.num_channels);
    if (s_alac == NULL) {
        ESP_LOGE(TAG, "alac_create failed");
        return -1;
    }
    s_alac->setinfo_max_samples_per_frame = c.frame_length;
    s_alac->setinfo_7a                    = c.compat_version;
    s_alac->setinfo_sample_size           = c.bit_depth;
    s_alac->setinfo_rice_historymult      = c.pb;
    s_alac->setinfo_rice_initialhistory   = c.mb;
    s_alac->setinfo_rice_kmodifier        = c.kb;
    s_alac->setinfo_7f                    = c.num_channels;
    s_alac->setinfo_80                    = c.max_run;
    s_alac->setinfo_82                    = c.max_frame_bytes;
    s_alac->setinfo_86                    = c.avg_bitrate;
    s_alac->setinfo_8a_rate               = c.sample_rate;
    alac_allocate_buffers(s_alac);

    s_num_channels = c.num_channels;
    s_frame_length = (uint16_t)c.frame_length;

    if (import_aes_key(s->aeskey) != 0) {
        teardown_crypto_decoder();
        return -1;
    }
    memcpy(s_iv, s->aesiv, sizeof(s_iv));

    rtp_reorder_init(&s_reorder, s_slots, REORDER_WINDOW, REORDER_HOLD);

    s_audio_fd   = s->audio_fd;
    s_control_fd = s->control_fd;
    s_timing_fd  = s->timing_fd;
    s_client_control_port = s->client_control_port;
    s_client_timing_port  = s->client_timing_port;

    if (s->client_ip != 0) {
        memset(&s_peer, 0, sizeof(s_peer));
        s_peer.sin_family = AF_INET;
        s_peer.sin_addr.s_addr = s->client_ip;
        s_peer_known = true;
    } else {
        s_peer_known = false;
    }

    if (!audio_producer_acquire("raop_rtp")) {
        teardown_crypto_decoder();
        return -1;
    }

    s_run  = true;
    s_done = false;
    k_thread_create(&s_rtp_thread, s_rtp_stack, K_THREAD_STACK_SIZEOF(s_rtp_stack),
                    rtp_task, NULL, NULL, NULL, 4, K_FP_REGS, K_NO_WAIT);
    k_thread_name_set(&s_rtp_thread, "raop_rtp");

    ESP_LOGI(TAG, "RTP receive started (audio=%d control=%d timing=%d)",
             s_audio_fd, s_control_fd, s_timing_fd);
    return 0;
}

bool raop_rtp_stop(void) {
    if (!s_run) return false;
    s_run = false;

    for (int i = 0; i < 50 && !s_done; i++) {
        k_sleep(K_MSEC(10));
    }

    audio_producer_release();
    teardown_crypto_decoder();

    s_audio_fd = s_control_fd = s_timing_fd = -1;
    ESP_LOGI(TAG, "RTP receive stopped");
    return true;
}
