/**
 * ==============================================================================
 * Wi-Fi Onboarding Captive Portal for RT-Spark AirPlay 1 Receiver
 * ==============================================================================
 * SoftAP + Zephyr DHCPv4 server + Wildcard Captive DNS + Micro HTTP Server.
 *
 * When active:
 * 1. The RW007 module spins up the open SoftAP "RT-Spark-Setup".
 * 2. Mobile phones (iOS / Android) and laptops receive 192.168.4.x via DHCP.
 * 3. The wildcard DNS responder intercepts all domain lookups and returns 192.168.4.1,
 *    triggering iOS / Android / macOS / Windows automatic captive popup dialogs.
 * 4. The micro HTTP server serves a dark-mode web page displaying an active Wi-Fi
 *    scan dropdown of nearby routers and an input for the Wi-Fi password.
 * 5. On POST /save, the credentials are captured, a confirmation response is served,
 *    and the portal shuts down cleanly to hand credentials to the main supervisor.
 * ==============================================================================
 */

#include "wifi_portal.h"
#include "rw007_wifi.h"
#include "rw007_netif.h"
#include "lcd_st7789.h"
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/dhcpv4_server.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#define SETUP_AP_SSID   "RT-Spark-Setup"
#define SETUP_PORTAL_IP "192.168.4.1"
#define REQ_MAX         768

static const char PAGE_OK[] =
    "<!DOCTYPE html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<style>body{background:#0F172A;color:#10B981;font-family:sans-serif;text-align:center;padding:40px}</style>"
    "</head><body><h2>Credentials Saved!</h2>"
    "<p style=color:#fff>The RT-Spark is joining your Wi-Fi network now...</p>"
    "<p style=color:#94A3B8>Reconnect your phone to your normal Wi-Fi network.</p>"
    "</body></html>";

static char s_ssid[WIFI_SSID_MAX_LEN];
static char s_pass[WIFI_PASS_MAX_LEN];
static bool s_got_creds = false;

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Extracts and URL-decodes one field from an application/x-www-form-urlencoded body. */
static bool form_field(const char *body, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = body;

    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            p += klen + 1;
            size_t o = 0;
            while (*p && *p != '&' && o + 1 < cap) {
                if (*p == '+') {
                    out[o++] = ' ';
                    p++;
                } else if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
                    out[o++] = (char)(hexval(p[1]) * 16 + hexval(p[2]));
                    p += 3;
                } else {
                    out[o++] = *p++;
                }
            }
            out[o] = '\0';
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

#define MAX_CONN     4
#define CONN_IDLE_MS 8000

struct conn {
    int fd;
    size_t len;
    int64_t deadline;
    char buf[REQ_MAX];
};

static struct conn s_conn[MAX_CONN];

static void send_all(int fd, const char *data, size_t len)
{
    int64_t give_up = k_uptime_get() + 4000;
    while (len > 0 && k_uptime_get() < give_up) {
        ssize_t n = zsock_send(fd, data, len, 0);
        if (n > 0) {
            data += n;
            len -= (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOMEM || errno == ENOBUFS)) {
            k_msleep(10);
        } else {
            return;
        }
    }
}

static void send_page(int fd, const char *body, size_t len)
{
    static char resp_buf[1800];
    int total = snprintf(resp_buf, sizeof(resp_buf),
                         "HTTP/1.1 200 OK\r\n"
                         "Content-Type: text/html; charset=utf-8\r\n"
                         "Content-Length: %u\r\n"
                         "Connection: close\r\n\r\n"
                         "%s", (unsigned)len, body);
    if (total > 0 && total < (int)sizeof(resp_buf)) {
        send_all(fd, resp_buf, (size_t)total);
    }
}

static bool request_complete(const char *req, size_t got)
{
    const char *hdr_end = strstr(req, "\r\n\r\n");
    if (!hdr_end) return false;
    if (strncmp(req, "POST", 4) != 0) return true;
    const char *cl = strstr(req, "Content-Length:");
    size_t want = cl ? (size_t)atoi(cl + 15) : 0;
    return got - (size_t)(hdr_end + 4 - req) >= want;
}

static void send_portal_page(int fd)
{
    rw007_ap_entry_t aps[RW007_MAX_SCAN_APS];
    int n_aps = rw007_wifi_get_scan_aps(aps, RW007_MAX_SCAN_APS);

    static char html_buf[1800];
    int len = snprintf(html_buf, sizeof(html_buf),
        "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>RT-Spark AirPlay Setup</title><style>"
        "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;background:#0b0f19;color:#fff;text-align:center;padding:20px;margin:0}"
        ".c{background:#161f30;border:1px solid #334155;border-radius:12px;padding:24px;max-width:340px;margin:auto;box-shadow:0 8px 24px rgba(0,0,0,0.5)}"
        "h2{color:#38bdf8;margin-top:0;font-size:20px}"
        "p{color:#94a3b8;font-size:13px;margin-bottom:16px}"
        "select,input,button{width:100%%;padding:12px;margin:8px 0;border-radius:8px;font-size:15px;box-sizing:border-box}"
        "select,input{border:1px solid #334155;background:#0f172a;color:#fff}"
        "button{background:#10b981;border:0;color:#fff;font-weight:bold;cursor:pointer;margin-top:12px}"
        "button:hover{background:#059669}"
        "</style></head><body><div class=\"c\">"
        "<h2>RT-Spark AirPlay 1</h2>"
        "<p>Select your home Wi-Fi network to connect.</p>"
        "<form method=\"POST\" action=\"/save\">"
        "<select onchange=\"if(this.value)document.getElementById('ssid').value=this.value;\">"
        "<option value=\"\">-- Select Wi-Fi (%d found) --</option>", n_aps);

    for (int i = 0; i < n_aps && len < (int)sizeof(html_buf) - 350; i++) {
        len += snprintf(html_buf + len, sizeof(html_buf) - len,
                        "<option value=\"%s\">%s (%d dBm)</option>",
                        aps[i].ssid, aps[i].ssid, aps[i].rssi);
    }

    len += snprintf(html_buf + len, sizeof(html_buf) - len,
        "</select>"
        "<input id=\"ssid\" name=\"ssid\" placeholder=\"Or enter SSID manually\" required>"
        "<input name=\"pass\" type=\"password\" placeholder=\"Wi-Fi Password\">"
        "<button type=\"submit\">Save &amp; Connect</button></form></div></body></html>");

    send_page(fd, html_buf, (size_t)len);
}

static void handle_request(int fd, char *req)
{
    char *eol = strstr(req, "\r\n");
    if (eol) {
        *eol = '\0';
        printk("[Portal HTTP] %s\n", req);
        *eol = '\r';
    }

    /* Intercept captive detection endpoints (iOS, macOS, Android, Windows) */
    if (strncmp(req, "GET /generate_204", 17) == 0 ||
        strncmp(req, "GET /gen_204", 12) == 0 ||
        strncmp(req, "GET /hotspot-detect.html", 24) == 0 ||
        strncmp(req, "GET /ncsi.txt", 13) == 0 ||
        strncmp(req, "GET /connecttest.txt", 20) == 0 ||
        strncmp(req, "GET /canonical.html", 19) == 0) {
        static const char REDIR[] =
            "HTTP/1.1 302 Found\r\n"
            "Location: http://192.168.4.1/\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        send_all(fd, REDIR, sizeof(REDIR) - 1);
        return;
    }

    if (strncmp(req, "POST /save", 10) == 0) {
        const char *body = strstr(req, "\r\n\r\n");
        if (body && form_field(body + 4, "ssid", s_ssid, sizeof(s_ssid)) && s_ssid[0]) {
            if (!form_field(body + 4, "pass", s_pass, sizeof(s_pass))) {
                s_pass[0] = '\0';
            }
            s_got_creds = true;
            printk("[Portal] Captured Wi-Fi credentials for SSID: '%s'\n", s_ssid);
            send_page(fd, PAGE_OK, sizeof(PAGE_OK) - 1);
            return;
        }
    }

    /* All other requests get the captive portal setup page */
    send_portal_page(fd);
}

static void conn_close(struct conn *c)
{
    if (c->fd >= 0) {
        zsock_close(c->fd);
        c->fd = -1;
    }
}

static void conn_accept(int lfd)
{
    int cfd = zsock_accept(lfd, NULL, NULL);
    struct conn *slot = NULL;
    if (cfd < 0) return;

    int nodelay = 1;
    zsock_setsockopt(cfd, NET_IPPROTO_TCP, ZSOCK_TCP_NODELAY, &nodelay, sizeof(nodelay));
    for (int i = 0; i < MAX_CONN; i++) {
        if (s_conn[i].fd < 0) {
            slot = &s_conn[i];
            break;
        }
    }
    if (!slot) {
        slot = &s_conn[0];
        for (int i = 1; i < MAX_CONN; i++) {
            if (s_conn[i].deadline < slot->deadline) slot = &s_conn[i];
        }
        conn_close(slot);
    }
    slot->fd = cfd;
    slot->len = 0;
    slot->buf[0] = '\0';
    slot->deadline = k_uptime_get() + CONN_IDLE_MS;
}

static void conn_service(struct conn *c)
{
    ssize_t n = zsock_recv(c->fd, c->buf + c->len, REQ_MAX - 1 - c->len, ZSOCK_MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        conn_close(c);
        return;
    }
    if (n > 0) {
        c->len += (size_t)n;
        c->buf[c->len] = '\0';
        if (request_complete(c->buf, c->len) || c->len >= REQ_MAX - 1) {
            handle_request(c->fd, c->buf);
            c->len = 0;
            zsock_shutdown(c->fd, ZSOCK_SHUT_WR);
            c->deadline = k_uptime_get() + 2500;
        }
    }
}

static int open_dns_listener(void)
{
    struct net_sockaddr_in a = { 0 };
    int one = 1;
    int fd = zsock_socket(NET_AF_INET, NET_SOCK_DGRAM, NET_IPPROTO_UDP);
    if (fd < 0) return -1;
    zsock_setsockopt(fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_REUSEADDR, &one, sizeof(one));
    a.sin_family = NET_AF_INET;
    a.sin_port = net_htons(53);
    if (zsock_bind(fd, (struct net_sockaddr *)&a, sizeof(a)) < 0) {
        zsock_close(fd);
        return -1;
    }
    return fd;
}

static void serve_dns_query(int fd)
{
    struct net_sockaddr_in client;
    net_socklen_t addrlen = sizeof(client);
    uint8_t buf[256];

    ssize_t len = zsock_recvfrom(fd, buf, sizeof(buf) - 16, ZSOCK_MSG_DONTWAIT,
                                 (struct net_sockaddr *)&client, &addrlen);
    if (len >= 12) {
        /* Standard DNS answer resolving any domain query to 192.168.4.1 */
        buf[2] = 0x81;
        buf[3] = 0x80;
        buf[6] = 0x00;
        buf[7] = 0x01;
        buf[8] = 0x00; buf[9] = 0x00;
        buf[10] = 0x00; buf[11] = 0x00;

        static const uint8_t ans[16] = {
            0xC0, 0x0C,             /* Pointer to QNAME at byte 12 */
            0x00, 0x01,             /* Type A */
            0x00, 0x01,             /* Class IN */
            0x00, 0x00, 0x00, 0x3C, /* TTL 60s */
            0x00, 0x04,             /* Data len 4 */
            192, 168, 4, 1          /* IP: 192.168.4.1 */
        };
        memcpy(buf + len, ans, sizeof(ans));
        zsock_sendto(fd, buf, len + sizeof(ans), 0,
                     (struct net_sockaddr *)&client, addrlen);
    }
}

static int open_listener(void)
{
    struct net_sockaddr_in a = { 0 };
    int one = 1;
    int fd = zsock_socket(NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TCP);
    if (fd < 0) return -1;
    zsock_setsockopt(fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_REUSEADDR, &one, sizeof(one));
    a.sin_family = NET_AF_INET;
    a.sin_port = net_htons(80);
    if (zsock_bind(fd, (struct net_sockaddr *)&a, sizeof(a)) < 0 || zsock_listen(fd, 2) < 0) {
        zsock_close(fd);
        return -1;
    }
    return fd;
}

bool wifi_portal_run(uint32_t timeout_sec, char *out_ssid, char *out_pass)
{
    struct net_if *iface = rw007_netif_get();
    struct net_in_addr ap_ip, mask, pool;

    printk("\n[PORTAL] Launching SoftAP '%s' -> http://%s\n", SETUP_AP_SSID, SETUP_PORTAL_IP);

    /* Render portal guidance on LCD */
    lcd_clear(LCD_COLOR_BLACK);
    lcd_fill_rect(0, 0, LCD_WIDTH - 1, 24, LCD_COLOR_NAVY);
    lcd_show_string(10, 4, "[Wi-Fi Setup Portal]", LCD_COLOR_WHITE, LCD_COLOR_NAVY);
    lcd_show_string(10, 45, "1. Connect phone to:", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
    lcd_show_string(20, 70, SETUP_AP_SSID, LCD_COLOR_YELLOW, LCD_COLOR_BLACK);
    lcd_show_string(10, 110, "2. Open in browser:", LCD_COLOR_GRAY, LCD_COLOR_BLACK);
    lcd_show_string(20, 135, "http://" SETUP_PORTAL_IP, LCD_COLOR_CYAN, LCD_COLOR_BLACK);
    lcd_show_string(10, 180, "Auto-popup supported", LCD_COLOR_DARKCYAN, LCD_COLOR_BLACK);

    if (!iface) {
        printk("[PORTAL] Error: No network interface!\n");
        return false;
    }

    if (!rw007_wifi_start_softap(SETUP_AP_SSID, NULL)) {
        printk("[PORTAL] Error: rw007_wifi_start_softap failed!\n");
        return false;
    }

    net_addr_pton(NET_AF_INET, "192.168.4.1", &ap_ip);
    net_addr_pton(NET_AF_INET, "255.255.255.0", &mask);
    net_addr_pton(NET_AF_INET, "192.168.4.2", &pool);

    {
        uint8_t ap_mac[6];
        rw007_wifi_get_ap_mac(ap_mac);
        rw007_netif_set_mac(ap_mac);
    }
    rw007_netif_link(true);

    if (!net_if_ipv4_addr_add(iface, &ap_ip, NET_ADDR_MANUAL, 0)) {
        printk("[PORTAL] Warning: Could not add 192.168.4.1 manually\n");
    }
    net_if_ipv4_set_netmask_by_addr(iface, &ap_ip, &mask);
    net_if_ipv4_set_gw(iface, &ap_ip);

    int rc = net_dhcpv4_server_start(iface, &pool);
    printk("[PORTAL] DHCPv4 server started (rc=%d, pool from 192.168.4.2)\n", rc);

    int lfd = open_listener();
    if (lfd < 0) {
        printk("[PORTAL] Error: Cannot open HTTP listener on port 80!\n");
        net_dhcpv4_server_stop(iface);
        rw007_wifi_stop_softap();
        return false;
    }
    printk("[PORTAL] HTTP server listening on TCP port 80\n");

    int dns_fd = open_dns_listener();
    if (dns_fd >= 0) {
        printk("[PORTAL] Wildcard captive DNS listening on UDP port 53\n");
    }

    /* Trigger a Wi-Fi scan so the dropdown is ready */
    rw007_wifi_scan();

    s_got_creds = false;
    for (int i = 0; i < MAX_CONN; i++) {
        s_conn[i].fd = -1;
    }

    int64_t end = timeout_sec ? k_uptime_get() + (int64_t)timeout_sec * 1000 : INT64_MAX;

    while (!s_got_creds && k_uptime_get() < end) {
        struct zsock_pollfd pfd[2 + MAX_CONN];
        int nfd = 0;

        pfd[0].fd = lfd;
        pfd[0].events = ZSOCK_POLLIN;
        pfd[0].revents = 0;
        nfd = 1;

        int dns_idx = -1;
        if (dns_fd >= 0) {
            dns_idx = nfd;
            pfd[nfd].fd = dns_fd;
            pfd[nfd].events = ZSOCK_POLLIN;
            pfd[nfd].revents = 0;
            nfd++;
        }

        for (int i = 0; i < MAX_CONN; i++) {
            if (s_conn[i].fd >= 0) {
                pfd[nfd].fd = s_conn[i].fd;
                pfd[nfd].events = ZSOCK_POLLIN;
                pfd[nfd].revents = 0;
                nfd++;
            }
        }

        if (zsock_poll(pfd, nfd, 200) > 0) {
            if (pfd[0].revents & ZSOCK_POLLIN) {
                conn_accept(lfd);
            }
            if (dns_idx >= 0 && (pfd[dns_idx].revents & ZSOCK_POLLIN)) {
                serve_dns_query(dns_fd);
            }
            for (int i = 0; i < MAX_CONN && !s_got_creds; i++) {
                if (s_conn[i].fd < 0) continue;
                for (int k = (dns_idx >= 0 ? 2 : 1); k < nfd; k++) {
                    if (pfd[k].fd == s_conn[i].fd && pfd[k].revents) {
                        conn_service(&s_conn[i]);
                        break;
                    }
                }
            }
        }

        for (int i = 0; i < MAX_CONN; i++) {
            if (s_conn[i].fd >= 0 && k_uptime_get() > s_conn[i].deadline) {
                conn_close(&s_conn[i]);
            }
        }
    }

    /* Teardown portal server */
    for (int i = 0; i < MAX_CONN; i++) {
        conn_close(&s_conn[i]);
    }
    zsock_close(lfd);
    if (dns_fd >= 0) zsock_close(dns_fd);

    k_msleep(1500); /* Allow client to receive confirmation page */

    net_dhcpv4_server_stop(iface);
    net_if_ipv4_addr_rm(iface, &ap_ip);
    rw007_netif_link(false);
    rw007_wifi_stop_softap();

    /* Restore Station MAC address for client mode */
    {
        uint8_t sta_mac[6];
        rw007_wifi_get_mac(sta_mac);
        rw007_netif_set_mac(sta_mac);
    }
    k_msleep(300);

    if (!s_got_creds) {
        printk("[PORTAL] Timed out without credentials.\n");
        return false;
    }

    strncpy(out_ssid, s_ssid, WIFI_SSID_MAX_LEN - 1);
    out_ssid[WIFI_SSID_MAX_LEN - 1] = '\0';
    strncpy(out_pass, s_pass, WIFI_PASS_MAX_LEN - 1);
    out_pass[WIFI_PASS_MAX_LEN - 1] = '\0';
    return true;
}
