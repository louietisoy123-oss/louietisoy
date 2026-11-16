#ifndef ESP_COMPAT_H_
#define ESP_COMPAT_H_

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/socket_select.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE -2

#define ESP_LOGI(tag, fmt, ...) printk("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printk("[%s][WARN] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printk("[%s][ERROR] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) do {} while (0)

static inline const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK: return "OK";
    case ESP_FAIL: return "FAIL";
    case ESP_ERR_INVALID_STATE: return "INVALID_STATE";
    default: return "ERROR";
    }
}

/* BSD Socket POSIX shim for Zephyr 4.x */
#ifndef socket
#define socket          zsock_socket
#define bind            zsock_bind
#define listen          zsock_listen
#define accept          zsock_accept
#define connect         zsock_connect
#define recv            zsock_recv
#define recvfrom        zsock_recvfrom
#define send            zsock_send
#define sendto          zsock_sendto
#define setsockopt      zsock_setsockopt
#define getsockopt      zsock_getsockopt
#define getsockname     zsock_getsockname
#define getpeername     zsock_getpeername
#define close           zsock_close
#define shutdown        zsock_shutdown
#define select          zsock_select
#define fd_set          zsock_fd_set
#define FD_ZERO         ZSOCK_FD_ZERO
#define FD_SET          ZSOCK_FD_SET
#define FD_ISSET        ZSOCK_FD_ISSET
#define FD_CLR          ZSOCK_FD_CLR
#define inet_pton       zsock_inet_pton
#define inet_ntop       zsock_inet_ntop
#endif

#ifdef __cplusplus
}
#endif

#endif /* ESP_COMPAT_H_ */
