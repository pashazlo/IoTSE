#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "wifi_port_scan.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Runtime MAP data lives in PSRAM and is independent of persistent media.
 * Saving is intentionally delegated to FM worker, whose volume abstraction
 * can target internal FAT today and an SD-card mount later.
 */
#define WIFI_MAP_STORE_FORMAT_VERSION 1U

typedef struct {
    uint32_t format_version;
    uint32_t generation;
    wifi_port_scan_result_t port_scan;
} wifi_map_snapshot_t;

esp_err_t wifi_map_store_init(void);
bool wifi_map_store_is_ready(void);
size_t wifi_map_store_size(void);

/* Writer API. Publishes one complete immutable generation. */
esp_err_t wifi_map_store_publish_port_scan(
    const wifi_port_scan_result_t *result,
    uint32_t *generation
);

/* Reader API. Copies a stable snapshot; never exposes PSRAM-owned pointers. */
bool wifi_map_store_get_snapshot(wifi_map_snapshot_t *snapshot);
bool wifi_map_store_get_port_scan(
    uint32_t expected_generation,
    wifi_port_scan_result_t *result
);

#ifdef __cplusplus
}
#endif
