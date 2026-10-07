#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WIFI_SCAN_OWNER_NONE = 0,
    WIFI_SCAN_OWNER_AUTO,
    WIFI_SCAN_OWNER_USER,
} wifi_scan_owner_t;

typedef struct {
    uint32_t active_id;
    uint32_t deadline_id;
    uint32_t retired_id;
    wifi_scan_owner_t owner;
} wifi_scan_lifecycle_t;

void wifi_scan_lifecycle_init(wifi_scan_lifecycle_t *scan);
bool wifi_scan_lifecycle_begin(wifi_scan_lifecycle_t *scan, uint32_t id,
                               wifi_scan_owner_t owner);
bool wifi_scan_lifecycle_preempt_auto(wifi_scan_lifecycle_t *scan,
                                      uint32_t *retired_id);
bool wifi_scan_lifecycle_event_is_current(const wifi_scan_lifecycle_t *scan,
                                          uint32_t id);
bool wifi_scan_lifecycle_deadline_is_current(const wifi_scan_lifecycle_t *scan);
void wifi_scan_lifecycle_complete(wifi_scan_lifecycle_t *scan);
