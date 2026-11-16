// AirPlay-1 (RAOP) RTSP control server for Zephyr RTOS on RT-Spark (STM32F407)
#include "raop.h"

#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/printk.h>

#include "esp_compat.h"
#include "rtsp_parser.h"
#include "sdp.h"
#include "base64.h"
#include "raop_challenge.h"
#include "raop_crypto.h"
#include "rtsp_session.h"
#include "raop_rtp.h"
#include "audio.h"
#include "mdns_service.h"
#include "setparam.h"
#include "raop_volume.h"
#include "dmap.h"
#include "raop_metadata.h"
#include "rw007_wifi.h"

static const char *TAG = "raop";

static raop_event_cb_t s_ev_cb = NULL;
void raop_set_event_cb(raop_event_cb_t cb) { s_ev_cb = cb; }

#define RAOP_RX_CAP 2048
#define RAOP_CLIENT_IDLE_MS 30000

K_THREAD_STACK_DEFINE(s_raop_stack, 4096);
static struct k_thread  s_raop_thread;
static volatile bool    s_running   = false;
static int              s_listen_fd = -1;
static raop_session_t   s_session;

/* Static buffers to prevent stack bloat in raop_rtsp thread */
static rtsp_request_t   s_rtsp_req;
static dmap_meta_t      s_dmap_meta;
static char             s_rtsp_out[1024];
static char             s_opt_extra[512];
static uint8_t          s_opt_sig[256];
static char             s_opt_b64[400];
static sdp_media_t      s_ann_media;
static uint8_t          s_ann_enc[256];
static char             s_setup_extra[256];
static char             s_rtsp_rx[RAOP_RX_CAP] __attribute__((section(".dtcm_noinit")));
static char             s_drain_tmp[256];

static void session_teardown_full(void) {
    if (raop_rtp_stop()) {
        /* Tone stopped */
    }
    raop_metadata_clear();
    raop_session_reset(&s_session);
    if (s_ev_cb) s_ev_cb(RAOP_EV_IDLE);
}

static void send_response(int fd, int status, const char *reason, int cseq,
                          const char *extra, const char *body, size_t body_len) {
    int n = rtsp_build_response(s_rtsp_out, sizeof(s_rtsp_out), status, reason,
                                cseq < 0 ? 0 : cseq, extra, body, body_len);
    if (n > 0) {
        send(fd, s_rtsp_out, (size_t)n, 0);
    } else {
        ESP_LOGE(TAG, "response build overflow (status %d)", status);
    }
}

static void handle_options(int fd, const rtsp_request_t *req, int cseq) {
    size_t eo = 0;
    int n = snprintf(s_opt_extra + eo, sizeof(s_opt_extra) - eo,
                     "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, "
                     "OPTIONS, GET_PARAMETER, SET_PARAMETER\r\n");
    if (n > 0 && (size_t)n < sizeof(s_opt_extra) - eo) eo += (size_t)n;

    const char *chal = rtsp_header_get(req, "Apple-Challenge");
    if (chal) {
        uint8_t cbuf[32];
        int clen = base64_decode(chal, strlen(chal), cbuf, sizeof(cbuf));
        if (clen == 16) {
            uint8_t ip4[4] = {0};
            struct sockaddr_in local;
            socklen_t ll = sizeof(local);
            if (getsockname(fd, (struct sockaddr *)&local, &ll) == 0)
                memcpy(ip4, &local.sin_addr.s_addr, 4);
            uint8_t mac[6] = {0};
            rw007_wifi_get_mac(mac);

            uint8_t buf32[RAOP_CHALLENGE_BUF_LEN];
            if (raop_challenge_assemble(cbuf, 16, ip4, mac, buf32) >= 0) {
                size_t siglen = 0;
                if (raop_crypto_sign_challenge(buf32, s_opt_sig, sizeof(s_opt_sig), &siglen) == 0) {
                    if (base64_encode(s_opt_sig, siglen, s_opt_b64, sizeof(s_opt_b64)) > 0) {
                        char *pad = strchr(s_opt_b64, '=');
                        if (pad) *pad = '\0';
                        int m = snprintf(s_opt_extra + eo, sizeof(s_opt_extra) - eo,
                                         "Apple-Response: %s\r\n", s_opt_b64);
                        if (m > 0 && (size_t)m < sizeof(s_opt_extra) - eo) eo += (size_t)m;
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "Apple-Challenge decoded to %d bytes (want 16)", clen);
        }
    }
    send_response(fd, 200, "OK", cseq, s_opt_extra, NULL, 0);
}

static void handle_announce(int fd, const rtsp_request_t *req, int cseq) {
    memset(&s_ann_media, 0, sizeof(s_ann_media));
    sdp_parse(req->body ? req->body : "", req->body_len, &s_ann_media);

    bool ok = true;

    if (s_ann_media.has_rsaaeskey) {
        int el = base64_decode(s_ann_media.rsaaeskey, strlen(s_ann_media.rsaaeskey), s_ann_enc, sizeof(s_ann_enc));
        if (el <= 0 || raop_crypto_decrypt_aeskey(s_ann_enc, (size_t)el, s_session.aeskey) != 0) {
            ESP_LOGW(TAG, "ANNOUNCE: rsaaeskey decrypt failed");
            ok = false;
        }
    } else {
        ok = false;
    }

    if (ok && s_ann_media.has_aesiv) {
        int il = base64_decode(s_ann_media.aesiv, strlen(s_ann_media.aesiv), s_session.aesiv, sizeof(s_session.aesiv));
        if (il != 16) {
            ESP_LOGW(TAG, "ANNOUNCE: aesiv decoded to %d bytes (want 16)", il);
            ok = false;
        }
    }

    if (ok) {
        s_session.have_key = s_ann_media.has_rsaaeskey && s_ann_media.has_aesiv;
        if (s_ann_media.has_fmtp) snprintf(s_session.fmtp, sizeof(s_session.fmtp), "%s", s_ann_media.fmtp);
        s_session.state = RAOP_ANNOUNCED;
        ESP_LOGI(TAG, "ANNOUNCE ok: AES key(16) decrypted, iv(16), fmtp=\"%s\"", s_session.fmtp);
        send_response(fd, 200, "OK", cseq, NULL, NULL, 0);
    } else {
        send_response(fd, 400, "Bad Request", cseq, NULL, NULL, 0);
    }
}

static void handle_setup(int fd, const rtsp_request_t *req, int cseq) {
    const char *t = rtsp_header_get(req, "Transport");
    if (t) {
        s_session.client_control_port = rtsp_transport_port(t, "control_port");
        s_session.client_timing_port  = rtsp_transport_port(t, "timing_port");
    }
    if (raop_session_bind_udp(&s_session) != 0) {
        send_response(fd, 500, "Internal Server Error", cseq, NULL, NULL, 0);
        return;
    }
    s_session.state = RAOP_SETUP;

    snprintf(s_setup_extra, sizeof(s_setup_extra),
             "Transport: RTP/AVP/UDP;unicast;mode=record;server_port=%u;"
             "control_port=%u;timing_port=%u\r\n"
             "Session: 1\r\n",
             s_session.audio_port, s_session.control_port, s_session.timing_port);
    ESP_LOGI(TAG, "SETUP ok: client control=%d timing=%d; our audio=%u control=%u timing=%u",
             s_session.client_control_port, s_session.client_timing_port,
             s_session.audio_port, s_session.control_port, s_session.timing_port);
    send_response(fd, 200, "OK", cseq, s_setup_extra, NULL, 0);
}

static void handle_set_parameter(int fd, const rtsp_request_t *req, int cseq) {
    const char *ct = rtsp_header_get(req, "Content-Type");
    switch (setparam_classify(ct)) {
        case SETPARAM_VOLUME: {
            float db;
            if (req->body && raop_parse_volume(req->body, req->body_len, &db)) {
                audio_airplay_set_volume(db);
            }
            uint32_t s, c, e;
            if (req->body && raop_parse_progress(req->body, req->body_len, &s, &c, &e)) {
                ESP_LOGI(TAG, "progress %us / %us", (unsigned)((c - s) / 44100),
                         (unsigned)((e - s) / 44100));
            }
            break;
        }
        case SETPARAM_METADATA: {
            if (req->body && req->body_len) {
                dmap_parse((const uint8_t *)req->body, req->body_len, &s_dmap_meta);
                raop_metadata_update(&s_dmap_meta);
            }
            break;
        }
        case SETPARAM_OTHER:
        default:
            break;
    }
    send_response(fd, 200, "OK", cseq, NULL, NULL, 0);
}

static bool dispatch(int fd, const rtsp_request_t *req) {
    int cseq = rtsp_cseq(req);
    ESP_LOGI(TAG, "%s (CSeq %d)", req->method, cseq);

    if (strcmp(req->method, "OPTIONS") == 0) {
        handle_options(fd, req, cseq);
    } else if (strcmp(req->method, "ANNOUNCE") == 0) {
        handle_announce(fd, req, cseq);
    } else if (strcmp(req->method, "SETUP") == 0) {
        handle_setup(fd, req, cseq);
    } else if (strcmp(req->method, "RECORD") == 0) {
        if (s_session.state == RAOP_RECORDING) {
            send_response(fd, 200, "OK", cseq, "Audio-Latency: 11025\r\n", NULL, 0);
        } else if (!s_session.have_key || s_session.fmtp[0] == '\0') {
            ESP_LOGW(TAG, "RECORD without key/fmtp -> 400");
            send_response(fd, 400, "Bad Request", cseq, NULL, NULL, 0);
        } else {
            audio_diag_tone_stop();
            struct sockaddr_in praddr;
            socklen_t prlen = sizeof(praddr);
            if (getpeername(fd, (struct sockaddr *)&praddr, &prlen) == 0) {
                s_session.client_ip = praddr.sin_addr.s_addr;
            }
            if (raop_rtp_start(&s_session) == 0) {
                s_session.state = RAOP_RECORDING;
                if (s_ev_cb) s_ev_cb(RAOP_EV_STREAMING);
                ESP_LOGI(TAG, "RECORD: RTP decode streaming (first AirPlay audio)");
            } else {
                ESP_LOGE(TAG, "RECORD: RTP start failed");
            }
            send_response(fd, 200, "OK", cseq, "Audio-Latency: 11025\r\n", NULL, 0);
        }
    } else if (strcmp(req->method, "SET_PARAMETER") == 0) {
        handle_set_parameter(fd, req, cseq);
    } else if (strcmp(req->method, "FLUSH") == 0 ||
               strcmp(req->method, "PAUSE") == 0 ||
               strcmp(req->method, "GET_PARAMETER") == 0) {
        send_response(fd, 200, "OK", cseq, NULL, NULL, 0);
    } else if (strcmp(req->method, "TEARDOWN") == 0) {
        send_response(fd, 200, "OK", cseq, NULL, NULL, 0);
        session_teardown_full();
        ESP_LOGI(TAG, "TEARDOWN: session released");
        return true;
    } else {
        send_response(fd, 501, "Not Implemented", cseq, NULL, NULL, 0);
    }
    return false;
}

static void configure_client_socket(int fd) {
    struct timeval rcv = { .tv_sec = RAOP_CLIENT_IDLE_MS / 1000, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
}

static int ci_startswith(const char *s, const char *key, size_t klen) {
    for (size_t i = 0; i < klen; i++) {
        char a = s[i], b = key[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static long hdr_scan_long(const char *buf, size_t len, const char *key) {
    size_t klen = strlen(key);
    if (len < klen) return -1;
    for (size_t i = 0; i + klen < len; i++) {
        if (ci_startswith(buf + i, key, klen)) {
            const char *v = buf + i + klen;
            const char *e = buf + len;
            while (v < e && (*v == ':' || *v == ' ' || *v == '\t')) v++;
            return strtol(v, NULL, 10);
        }
    }
    return -1;
}

static void server_task(void *a1, void *a2, void *a3) {
    ARG_UNUSED(a1); ARG_UNUSED(a2); ARG_UNUSED(a3);

    if (raop_crypto_init() != ESP_OK) {
        ESP_LOGE(TAG, "raop_crypto_init failed; RTSP server not serving");
        s_running = false;
        while (1) { k_sleep(K_FOREVER); }
    }

    s_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen_fd < 0) {
        ESP_LOGE(TAG, "listen socket() failed: errno %d", errno);
        s_running = false;
        while (1) { k_sleep(K_FOREVER); }
    }
    int one = 1;
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(RAOP_RTSP_PORT);
    if (bind(s_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind :%d failed: errno %d", RAOP_RTSP_PORT, errno);
        close(s_listen_fd);
        s_listen_fd = -1;
        s_running = false;
        while (1) { k_sleep(K_FOREVER); }
    }
    listen(s_listen_fd, 2);
    ESP_LOGI(TAG, "RTSP listening on :%d", RAOP_RTSP_PORT);

    int       client_fd = -1;
    size_t    used = 0;
    uint32_t  last_activity = 0;

    while (s_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s_listen_fd, &rfds);
        int maxfd = s_listen_fd;
        if (client_fd >= 0) {
            FD_SET(client_fd, &rfds);
            if (client_fd > maxfd) maxfd = client_fd;
        }
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            ESP_LOGW(TAG, "select failed: errno %d (recovering)", errno);
            if (client_fd >= 0) {
                close(client_fd);
                client_fd = -1;
                session_teardown_full();
                continue;
            }
            k_sleep(K_MSEC(100));
            continue;
        }

        if (client_fd >= 0 &&
            (k_uptime_get_32() - last_activity) >= RAOP_CLIENT_IDLE_MS) {
            ESP_LOGW(TAG, "RTSP client idle > %d ms -> teardown", RAOP_CLIENT_IDLE_MS);
            close(client_fd);
            client_fd = -1;
            session_teardown_full();
            continue;
        }

        if (r == 0) continue;

        if (FD_ISSET(s_listen_fd, &rfds)) {
            struct sockaddr_in cli;
            socklen_t cl = sizeof(cli);
            int nfd = accept(s_listen_fd, (struct sockaddr *)&cli, &cl);
            if (nfd >= 0) {
                if (client_fd >= 0) {
                    int bn = rtsp_build_response(s_rtsp_out, sizeof(s_rtsp_out), 453,
                                                 "Not Enough Bandwidth", 0, NULL, NULL, 0);
                    if (bn > 0) send(nfd, s_rtsp_out, (size_t)bn, 0);
                    close(nfd);
                    ESP_LOGW(TAG, "second sender refused (453 busy)");
                } else {
                    client_fd = nfd;
                    used = 0;
                    last_activity = k_uptime_get_32();
                    configure_client_socket(client_fd);
                    session_teardown_full();
                    ESP_LOGI(TAG, "sender connected");
                }
            }
        }

        if (client_fd >= 0 && FD_ISSET(client_fd, &rfds)) {
            if (used >= sizeof(s_rtsp_rx)) {
                const char *hdr_end = NULL;
                for (size_t i = 0; i + 4 <= used; i++) {
                    if (memcmp(s_rtsp_rx + i, "\r\n\r\n", 4) == 0) { hdr_end = s_rtsp_rx + i + 4; break; }
                }
                if (hdr_end) {
                    size_t hlen = (size_t)(hdr_end - s_rtsp_rx);
                    long clen = hdr_scan_long(s_rtsp_rx, hlen, "Content-Length");
                    long cseq = hdr_scan_long(s_rtsp_rx, hlen, "CSeq");
                    long to_drain = (clen > 0) ? clen - (long)(used - hlen) : 0;
                    while (to_drain > 0) {
                        size_t want = (to_drain < (long)sizeof(s_drain_tmp)) ? (size_t)to_drain : sizeof(s_drain_tmp);
                        int dn = recv(client_fd, s_drain_tmp, want, 0);
                        if (dn <= 0) break;
                        to_drain -= dn;
                    }
                    ESP_LOGW(TAG, "oversized request (Content-Length=%ld) drained -> 200 OK", clen);
                    send_response(client_fd, 200, "OK", (int)(cseq < 0 ? 0 : cseq), NULL, NULL, 0);
                    used = 0;
                    last_activity = k_uptime_get_32();
                    continue;
                }
                ESP_LOGW(TAG, "request headers exceed %d bytes -> 400", (int)sizeof(s_rtsp_rx));
                send_response(client_fd, 400, "Bad Request", 0, NULL, NULL, 0);
                close(client_fd);
                client_fd = -1;
                session_teardown_full();
                continue;
            }
            int n = recv(client_fd, s_rtsp_rx + used, sizeof(s_rtsp_rx) - used, 0);
            if (n <= 0) {
                ESP_LOGI(TAG, "sender disconnected");
                close(client_fd);
                client_fd = -1;
                session_teardown_full();
                continue;
            }
            used += (size_t)n;
            last_activity = k_uptime_get_32();

            bool close_conn = false;
            for (;;) {
                if (!rtsp_parse_request(s_rtsp_rx, used, &s_rtsp_req)) break;
                close_conn = dispatch(client_fd, &s_rtsp_req);
                size_t consumed = s_rtsp_req.total_len;
                if (consumed == 0 || consumed > used) consumed = used;
                memmove(s_rtsp_rx, s_rtsp_rx + consumed, used - consumed);
                used -= consumed;
                if (close_conn) break;
            }
            if (close_conn) {
                close(client_fd);
                client_fd = -1;
            }
        }
    }

    if (client_fd >= 0) close(client_fd);
    if (s_listen_fd >= 0) {
        close(s_listen_fd);
        s_listen_fd = -1;
    }
    session_teardown_full();
    ESP_LOGI(TAG, "RTSP server stopped");
    while (1) { k_sleep(K_FOREVER); }
}

void raop_server_start(void) {
    if (s_running) {
        ESP_LOGW(TAG, "raop_server_start: already running");
        return;
    }
    raop_session_reset(&s_session);
    s_running = true;
    k_thread_create(&s_raop_thread, s_raop_stack, K_THREAD_STACK_SIZEOF(s_raop_stack),
                    server_task, NULL, NULL, NULL, 5, K_FP_REGS, K_NO_WAIT);
    k_thread_name_set(&s_raop_thread, "raop_rtsp");
}

void raop_server_stop(void) {
    s_running = false;
    if (s_listen_fd >= 0) {
        close(s_listen_fd);
        s_listen_fd = -1;
    }
}
