#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_HOST_DISCOVERY_MAX_HOSTS 32U
#define WIFI_HOST_DISCOVERY_REPLY_WINDOW_MS 800U

typedef enum {
    WIFI_HOST_DISCOVERY_SOURCE_ICMP = 0,
    /* The connected gateway is known reachable even if it ignores ICMP. */
    WIFI_HOST_DISCOVERY_SOURCE_GATEWAY,
} wifi_host_discovery_source_t;

typedef struct {
    uint32_t ipv4;
    uint8_t ttl;
    wifi_host_discovery_source_t source;
} wifi_discovered_host_t;

typedef struct {
    esp_err_t error;
    bool attempted;
    bool complete;
    bool truncated;
    uint16_t candidate_count;
    uint16_t request_count;
    uint16_t reply_count;
    uint16_t dropped_count;
    uint8_t host_count;
    wifi_discovered_host_t hosts[WIFI_HOST_DISCOVERY_MAX_HOSTS];
} wifi_host_discovery_result_t;

typedef bool (*wifi_host_discovery_abort_fn)(void *context);

/*
 * Finds the bounded IPv4 window used for discovery. Networks wider than /24
 * are restricted to the /24 containing local_ipv4. Returned addresses and
 * inputs use the same network byte order as wifi_connection_info_t.
 */
bool wifi_host_discovery_bounds(uint32_t local_ipv4,
                                uint32_t netmask,
                                uint32_t *first_host,
                                uint32_t *last_host);

/*
 * One raw ICMP socket, no per-host allocations and no additional task.
 * Missing replies mean "not observed", never "host is down".
 */
esp_err_t wifi_host_discovery_run(
    const wifi_connection_info_t *connection,
    wifi_host_discovery_result_t *result,
    wifi_host_discovery_abort_fn should_abort,
    void *abort_context
);

#ifdef __cplusplus
}
#endif
