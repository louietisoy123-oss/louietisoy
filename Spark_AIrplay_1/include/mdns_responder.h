#ifndef MDNS_RESPONDER_H_
#define MDNS_RESPONDER_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and start the lightweight mDNS responder thread for AirPlay discovery.
 *
 * Joins multicast group 224.0.0.251 on UDP port 5353 and advertises _raop._tcp.
 *
 * @param hostname Device hostname (e.g. "rtspark")
 * @param instance_name Service instance name (e.g. "001122334455@RT-Spark Hi-Fi")
 * @param port RAOP RTSP port (typically 5000)
 * @param ip_addr Device IPv4 address
 * @return true on success, false on failure.
 */
bool mdns_responder_start(const char *hostname, const char *instance_name, uint16_t port, uint32_t ip_addr);

/**
 * @brief Stop the mDNS responder.
 */
void mdns_responder_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* MDNS_RESPONDER_H_ */
