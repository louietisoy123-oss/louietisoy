#include "mdns_responder.h"
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/igmp.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "esp_compat.h"

#define MDNS_PORT           5353
#define MDNS_MCAST_ADDR     "224.0.0.251"
#define MDNS_BUF_SIZE       1024

static int s_mdns_fd = -1;
static volatile bool s_mdns_running = false;
static char s_hostname[32] = "rtspark";
static char s_instance[64] = "001122334455@RT-Spark Hi-Fi";
static uint16_t s_port = 5000;
static uint32_t s_ip = 0;

static uint8_t s_resp_buf[MDNS_BUF_SIZE];
static uint8_t s_rx_buf[1024];
static char s_full_raop[128];
static char s_full_airplay[128];
static char s_target_host[64];
static char s_devid_str[48];
static struct sockaddr_in s_mcast_dest;
static struct sockaddr_in s_query_src;

K_THREAD_STACK_DEFINE(s_mdns_stack, 4096);
static struct k_thread s_mdns_thread;

static bool memcasecmp(const uint8_t *buf, const char *str, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t c1 = buf[i];
        uint8_t c2 = (uint8_t)str[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return false;
    }
    return true;
}

static void write_label(uint8_t **buf, const char *str)
{
    const char *p = str;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        *(*buf)++ = (uint8_t)len;
        memcpy(*buf, p, len);
        *buf += len;
        if (!dot) break;
        p = dot + 1;
    }
    *(*buf)++ = 0x00;
}

static size_t build_mdns_response(uint8_t *out, size_t max_len)
{
    if (max_len < 800) return 0;
    uint8_t *p = out;

    /* Extract human-readable display name and format colon-separated MAC */
    const char *at = strchr(s_instance, '@');
    const char *disp_name = at ? (at + 1) : s_instance;

    /* Build "deviceid=xx:xx:xx:xx:xx:xx" directly without snprintf/malloc */
    memcpy(s_devid_str, "deviceid=", 9);
    char *d = &s_devid_str[9];
    for (int i = 0; i < 12; i++) {
        char c = s_instance[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        *d++ = c;
        if ((i % 2 == 1) && (i < 11)) {
            *d++ = ':';
        }
    }
    *d = '\0';

    /* Build service domain names */
    strncpy(s_full_raop, s_instance, sizeof(s_full_raop) - 18);
    s_full_raop[sizeof(s_full_raop) - 18] = '\0';
    strcat(s_full_raop, "._raop._tcp.local");

    strncpy(s_full_airplay, disp_name, sizeof(s_full_airplay) - 21);
    s_full_airplay[sizeof(s_full_airplay) - 21] = '\0';
    strcat(s_full_airplay, "._airplay._tcp.local");

    strncpy(s_target_host, s_hostname, sizeof(s_target_host) - 7);
    s_target_host[sizeof(s_target_host) - 7] = '\0';
    strcat(s_target_host, ".local");

    /* DNS Header */
    *p++ = 0x00; *p++ = 0x00; /* ID 0 */
    *p++ = 0x84; *p++ = 0x00; /* Flags: Response, Authoritative */
    *p++ = 0x00; *p++ = 0x00; /* QDCOUNT = 0 */
    *p++ = 0x00; *p++ = 0x09; /* ANCOUNT = 9 */
    *p++ = 0x00; *p++ = 0x00; /* NSCOUNT = 0 */
    *p++ = 0x00; *p++ = 0x00; /* ARCOUNT = 0 */

    uint8_t *rdlen_ptr;
    uint8_t *data_start;
    uint16_t rdlen;

    /* 1. Answer: PTR _services._dns-sd._udp.local -> _raop._tcp.local */
    write_label(&p, "_services._dns-sd._udp.local");
    *p++ = 0x00; *p++ = 0x0C; /* TYPE = PTR */
    *p++ = 0x00; *p++ = 0x01; /* CLASS = IN (shared, no cache-flush) */
    *p++ = 0x00; *p++ = 0x00; *p++ = 0x11; *p++ = 0x94; /* TTL = 4500s */
    rdlen_ptr = p; p += 2;
    data_start = p;
    write_label(&p, "_raop._tcp.local");
    rdlen = (uint16_t)(p - data_start);
    rdlen_ptr[0] = (uint8_t)(rdlen >> 8);
    rdlen_ptr[1] = (uint8_t)(rdlen & 0xFF);

    /* 2. Answer: PTR _services._dns-sd._udp.local -> _airplay._tcp.local */
    write_label(&p, "_services._dns-sd._udp.local");
    *p++ = 0x00; *p++ = 0x0C; /* TYPE = PTR */
    *p++ = 0x00; *p++ = 0x01; /* CLASS = IN (shared, no cache-flush) */
    *p++ = 0x00; *p++ = 0x00; *p++ = 0x11; *p++ = 0x94; /* TTL = 4500s */
    rdlen_ptr = p; p += 2;
    data_start = p;
    write_label(&p, "_airplay._tcp.local");
    rdlen = (uint16_t)(p - data_start);
    rdlen_ptr[0] = (uint8_t)(rdlen >> 8);
    rdlen_ptr[1] = (uint8_t)(rdlen & 0xFF);

    /* 3. Answer: PTR _raop._tcp.local -> <s_full_raop> */
    write_label(&p, "_raop._tcp.local");
    *p++ = 0x00; *p++ = 0x0C; /* TYPE = PTR */
    *p++ = 0x00; *p++ = 0x01; /* CLASS = IN (shared, no cache-flush) */
    *p++ = 0x00; *p++ = 0x00; *p++ = 0x11; *p++ = 0x94; /* TTL = 4500s */
    rdlen_ptr = p; p += 2;
    data_start = p;
    write_label(&p, s_full_raop);
    rdlen = (uint16_t)(p - data_start);
    rdlen_ptr[0] = (uint8_t)(rdlen >> 8);
    rdlen_ptr[1] = (uint8_t)(rdlen & 0xFF);

    /* 4. Answer: TXT <s_full_raop> */
    write_label(&p, s_full_raop);
    *p++ = 0x00; *p++ = 0x10; /* TYPE = TXT */
    *p++ = 0x80; *p++ = 0x01; /* CLASS = IN (cache-flush) */
    *p++ = 0x00; *p++ = 0x00; *p++ = 0x11; *p++ = 0x94; /* TTL = 4500s */
    static const char *raop_txt_records[] = {
        "txtvers=1",
        "ch=2",
        "cn=0,1",
        "et=0,1",
        "sv=false",
        "da=true",
        "sr=44100",
        "ss=16",
        "pw=false",
        "vn=3",
        "tp=UDP",
        "md=0,1,2",
        "vs=220.68",
        "am=AirPort10,115"
    };
    rdlen_ptr = p; p += 2;
    data_start = p;
    for (size_t i = 0; i < sizeof(raop_txt_records)/sizeof(raop_txt_records[0]); i++) {
        size_t slen = strlen(raop_txt_records[i]);
        *p++ = (uint8_t)slen;
        memcpy(p, raop_txt_records[i], slen);
        p += slen;
    }
    rdlen = (uint16_t)(p - data_start);
    rdlen_ptr[0] = (uint8_t)(rdlen >> 8);
    rdlen_ptr[1] = (uint8_t)(rdlen & 0xFF);

    /* 5. Answer: SRV <s_full_raop> -> port 5000 on <hostname>.local */
    write_label(&p, s_full_raop);
    *p++ = 0x00; *p++ = 0x21; /* TYPE = SRV */
    *p++ = 0x80; *p++ = 0x01; /* CLASS = IN (cache-flush) */
    *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x78; /* TTL = 120s */
    rdlen_ptr = p; p += 2;
    data_start = p;
    *p++ = 0x00; *p++ = 0x00; /* Priority 0 */
    *p++ = 0x00; *p++ = 0x00; /* Weight 0 */
    *p++ = (uint8_t)(s_port >> 8); *p++ = (uint8_t)(s_port & 0xFF); /* Port */
    write_label(&p, s_target_host);
    rdlen = (uint16_t)(p - data_start);
    rdlen_ptr[0] = (uint8_t)(rdlen >> 8);
    rdlen_ptr[1] = (uint8_t)(rdlen & 0xFF);

    /* 6. Answer: PTR _airplay._tcp.local -> <s_full_airplay> */
    write_label(&p, "_airplay._tcp.local");
    *p++ = 0x00; *p++ = 0x0C; /* TYPE = PTR */
    *p++ = 0x00; *p++ = 0x01; /* CLASS = IN (shared, no cache-flush) */
    *p++ = 0x00; *p++ = 0x00; *p++ = 0x11; *p++ = 0x94; /* TTL = 4500s */
    rdlen_ptr = p; p += 2;
    data_start = p;
    write_label(&p, s_full_airplay);
    rdlen = (uint16_t)(p - data_start);
    rdlen_ptr[0] = (uint8_t)(rdlen >> 8);
    rdlen_ptr[1] = (uint8_t)(rdlen & 0xFF);

    /* 7. Answer: TXT <s_full_airplay> */
    write_label(&p, s_full_airplay);
    *p++ = 0x00; *p++ = 0x10; /* TYPE = TXT */
    *p++ = 0x80; *p++ = 0x01; /* CLASS = IN (cache-flush) */
    *p++ = 0x00; *p++ = 0x00; *p++ = 0x11; *p++ = 0x94; /* TTL = 4500s */
    const char *airplay_txt_records[] = {
        s_devid_str,
        "features=0x5A7FFFF7",
        "flags=0x4",
        "model=AirPort10,115",
        "srcvers=220.68",
        "vv=2"
    };
    rdlen_ptr = p; p += 2;
    data_start = p;
    for (size_t i = 0; i < sizeof(airplay_txt_records)/sizeof(airplay_txt_records[0]); i++) {
        size_t slen = strlen(airplay_txt_records[i]);
        *p++ = (uint8_t)slen;
        memcpy(p, airplay_txt_records[i], slen);
        p += slen;
    }
    rdlen = (uint16_t)(p - data_start);
    rdlen_ptr[0] = (uint8_t)(rdlen >> 8);
    rdlen_ptr[1] = (uint8_t)(rdlen & 0xFF);

    /* 8. Answer: SRV <s_full_airplay> -> port 5000 on <hostname>.local */
    write_label(&p, s_full_airplay);
    *p++ = 0x00; *p++ = 0x21; /* TYPE = SRV */
    *p++ = 0x80; *p++ = 0x01; /* CLASS = IN (cache-flush) */
    *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x78; /* TTL = 120s */
    rdlen_ptr = p; p += 2;
    data_start = p;
    *p++ = 0x00; *p++ = 0x00; /* Priority 0 */
    *p++ = 0x00; *p++ = 0x00; /* Weight 0 */
    *p++ = (uint8_t)(s_port >> 8); *p++ = (uint8_t)(s_port & 0xFF); /* Port */
    write_label(&p, s_target_host);
    rdlen = (uint16_t)(p - data_start);
    rdlen_ptr[0] = (uint8_t)(rdlen >> 8);
    rdlen_ptr[1] = (uint8_t)(rdlen & 0xFF);

    /* 9. Answer: A <hostname>.local -> IP */
    write_label(&p, s_target_host);
    *p++ = 0x00; *p++ = 0x01; /* TYPE = A */
    *p++ = 0x80; *p++ = 0x01; /* CLASS = IN (cache-flush) */
    *p++ = 0x00; *p++ = 0x00; *p++ = 0x00; *p++ = 0x78; /* TTL = 120s */
    *p++ = 0x00; *p++ = 0x04; /* RDLENGTH = 4 */
    memcpy(p, &s_ip, 4);
    p += 4;

    return (size_t)(p - out);
}

static void mdns_worker(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

    memset(&s_mcast_dest, 0, sizeof(s_mcast_dest));
    s_mcast_dest.sin_family = AF_INET;
    s_mcast_dest.sin_port = htons(MDNS_PORT);
    inet_pton(AF_INET, MDNS_MCAST_ADDR, &s_mcast_dest.sin_addr);

    size_t resp_len = build_mdns_response(s_resp_buf, sizeof(s_resp_buf));
    printk("[mDNS] Built mDNS response packet: %zu bytes\n", resp_len);

    /* Broadcast initial announcement 3 times */
    for (int i = 0; i < 3 && s_mdns_running; i++) {
        if (resp_len > 0) {
            sendto(s_mdns_fd, s_resp_buf, resp_len, 0, (struct sockaddr *)&s_mcast_dest, sizeof(s_mcast_dest));
        }
        k_sleep(K_MSEC(250));
    }

    uint32_t last_announce = k_uptime_get_32();

    while (s_mdns_running) {
        socklen_t srclen = sizeof(s_query_src);
        memset(&s_query_src, 0, sizeof(s_query_src));
        int n = recvfrom(s_mdns_fd, s_rx_buf, sizeof(s_rx_buf), 0, (struct sockaddr *)&s_query_src, &srclen);

        uint32_t now = k_uptime_get_32();

        if (n > 12) {
            printk("[mDNS] RX %d bytes from %u.%u.%u.%u:%u (fam=%d)\n",
                   n,
                   (uint8_t)(s_query_src.sin_addr.s_addr & 0xFF),
                   (uint8_t)((s_query_src.sin_addr.s_addr >> 8) & 0xFF),
                   (uint8_t)((s_query_src.sin_addr.s_addr >> 16) & 0xFF),
                   (uint8_t)((s_query_src.sin_addr.s_addr >> 24) & 0xFF),
                   ntohs(s_query_src.sin_port),
                   s_query_src.sin_family);

            bool matches = false;

            /* Check if query targets our services or general discovery */
            for (int i = 0; i <= n - 5; i++) {
                if (memcasecmp(&s_rx_buf[i], "_raop", 5) ||
                    (i <= n - 8 && memcasecmp(&s_rx_buf[i], "_airplay", 8)) ||
                    (i <= n - 9 && memcasecmp(&s_rx_buf[i], "_services", 9)) ||
                    (i <= n - 7 && memcasecmp(&s_rx_buf[i], "rtspark", 7)) ||
                    (i <= n - 8 && memcasecmp(&s_rx_buf[i], "rt-spark", 8))) {
                    matches = true;
                    break;
                }
            }

            if (matches && resp_len > 0) {
                /* Ensure sin_family is AF_INET */
                s_query_src.sin_family = AF_INET;
                /* Reply unicast directly to the sender first (bypasses any Wi-Fi AP multicast drops) */
                if (s_query_src.sin_addr.s_addr != 0 &&
                    s_query_src.sin_addr.s_addr != s_mcast_dest.sin_addr.s_addr) {
                    sendto(s_mdns_fd, s_resp_buf, resp_len, 0, (struct sockaddr *)&s_query_src, sizeof(s_query_src));
                }

                /* Also reply multicast to 224.0.0.251 */
                sendto(s_mdns_fd, s_resp_buf, resp_len, 0, (struct sockaddr *)&s_mcast_dest, sizeof(s_mcast_dest));

                printk("[mDNS] Replied to %u.%u.%u.%u:%u\n",
                       (uint8_t)(s_query_src.sin_addr.s_addr & 0xFF),
                       (uint8_t)((s_query_src.sin_addr.s_addr >> 8) & 0xFF),
                       (uint8_t)((s_query_src.sin_addr.s_addr >> 16) & 0xFF),
                       (uint8_t)((s_query_src.sin_addr.s_addr >> 24) & 0xFF),
                       ntohs(s_query_src.sin_port));
            }
        }

        /* Periodic unsolicited announcement every 3 seconds for instant discovery */
        if (now - last_announce >= 3000) {
            last_announce = now;
            if (resp_len > 0) {
                sendto(s_mdns_fd, s_resp_buf, resp_len, 0, (struct sockaddr *)&s_mcast_dest, sizeof(s_mcast_dest));
            }
        }
    }

    while (1) {
        k_sleep(K_FOREVER);
    }
}

bool mdns_responder_start(const char *hostname, const char *instance_name, uint16_t port, uint32_t ip_addr)
{
    if (s_mdns_running) return true;

    if (hostname) strncpy(s_hostname, hostname, sizeof(s_hostname) - 1);
    if (instance_name) strncpy(s_instance, instance_name, sizeof(s_instance) - 1);
    s_port = port;
    s_ip = ip_addr;

    s_mdns_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_mdns_fd < 0) {
        printk("[mDNS] socket() failed: %d\n", errno);
        return false;
    }

    int reuse = 1;
    setsockopt(s_mdns_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /* Set socket receive timeout to 5 seconds so worker thread can wake periodically */
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(s_mdns_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(MDNS_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s_mdns_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        printk("[mDNS] bind() failed: %d\n", errno);
        close(s_mdns_fd);
        s_mdns_fd = -1;
        return false;
    }

    /* Join 224.0.0.251 multicast group at socket layer using struct net_ip_mreqn */
    struct net_ip_mreqn mreqn;
    memset(&mreqn, 0, sizeof(mreqn));
    inet_pton(AF_INET, MDNS_MCAST_ADDR, &mreqn.imr_multiaddr);
    mreqn.imr_address.s_addr = htonl(INADDR_ANY);
    mreqn.imr_ifindex = 0;

    int ret = setsockopt(s_mdns_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreqn, sizeof(mreqn));
    if (ret < 0) {
        printk("[mDNS] setsockopt(IP_ADD_MEMBERSHIP) failed: %d\n", errno);
    } else {
        printk("[mDNS] setsockopt(IP_ADD_MEMBERSHIP) OK\n");
    }

    /* Also join directly on default interface */
    struct net_if *iface = net_if_get_default();
    if (iface) {
        struct net_in_addr mcast_addr;
        inet_pton(AF_INET, MDNS_MCAST_ADDR, &mcast_addr);
        ret = net_ipv4_igmp_join(iface, &mcast_addr, NULL);
        printk("[mDNS] net_ipv4_igmp_join -> %d\n", ret);
    }

    s_mdns_running = true;
    k_thread_create(&s_mdns_thread, s_mdns_stack, K_THREAD_STACK_SIZEOF(s_mdns_stack),
                    mdns_worker, NULL, NULL, NULL, 6, K_FP_REGS, K_NO_WAIT);
    k_thread_name_set(&s_mdns_thread, "mdns_responder");

    printk("[mDNS] AirPlay mDNS responder active for '%s' on %s.local:%u\n",
           s_instance, s_hostname, s_port);
    return true;
}

void mdns_responder_stop(void)
{
    if (!s_mdns_running) return;
    s_mdns_running = false;
    if (s_mdns_fd >= 0) {
        close(s_mdns_fd);
        s_mdns_fd = -1;
    }
}
