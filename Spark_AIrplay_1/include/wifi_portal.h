/**
 * @file wifi_portal.h
 * @brief Captive Wi-Fi onboarding portal for RT-Spark AirPlay 1 receiver:
 *        SoftAP "RT-Spark-Setup" + DHCPv4 server + Captive DNS + Web Configuration Form.
 */

#ifndef WIFI_PORTAL_H_
#define WIFI_PORTAL_H_

#include <stdint.h>
#include <stdbool.h>
#include "wifi_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Runs the captive portal until credentials are submitted via HTTP form or timeout expires.
 *
 * Starts the open SoftAP "RT-Spark-Setup", DHCPv4 server, wildcard captive DNS
 * responder (UDP 53 -> 192.168.4.1), and an HTTP server (TCP 80).
 * Serves an SSID scan dropdown + password form, intercepts OS captive probes,
 * and captures submitted credentials.
 *
 * @param timeout_sec Timeout window in seconds (e.g. 120s; 0 waits indefinitely).
 * @param out_ssid    Buffer of size WIFI_SSID_MAX_LEN to receive submitted SSID.
 * @param out_pass    Buffer of size WIFI_PASS_MAX_LEN to receive submitted password.
 * @return true if credentials were submitted and parsed, false if timed out.
 */
bool wifi_portal_run(uint32_t timeout_sec, char *out_ssid, char *out_pass);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_PORTAL_H_ */
