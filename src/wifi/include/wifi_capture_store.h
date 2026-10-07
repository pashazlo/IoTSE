#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "wifi_promiscuous.h"

#define WIFI_CAPTURE_BANK_BYTES       (512U * 1024U)
#define WIFI_CAPTURE_BANK_COUNT       2U
#define WIFI_CAPTURE_MAX_RECORDS      2048U
#define WIFI_CAPTURE_PSRAM_BUDGET     \
    (WIFI_CAPTURE_BANK_BYTES * WIFI_CAPTURE_BANK_COUNT)

typedef struct {
    uint32_t capacity_bytes;
    uint32_t used_bytes;
    uint32_t record_count;
    uint32_t high_water_bytes;
    uint32_t high_water_records;
    uint32_t overwritten_records;
    uint32_t overwritten_bytes;
    uint32_t dropped_busy;
    uint32_t dropped_frozen;
    uint32_t dropped_oversize;
    uint32_t corruption_resets;
    uint32_t save_busy_count;
    uint32_t correlation_id;
    uint64_t next_sequence;
    bool frozen;
    bool saving;
    uint8_t active_bank;
    int8_t sealed_bank;
    uint32_t writer_stack_hwm;
} wifi_capture_store_snapshot_t;

typedef enum {
    WIFI_CAPTURE_SAVE_QUEUED = 0,
    WIFI_CAPTURE_SAVE_EMPTY,
    WIFI_CAPTURE_SAVE_BUSY,
    WIFI_CAPTURE_SAVE_ERROR,
} wifi_capture_save_status_t;

esp_err_t wifi_capture_store_init(void);
void wifi_capture_store_reset(void);
bool wifi_capture_store_append(const wifi_raw_packet_t *packet);
void wifi_capture_store_set_correlation_id(uint32_t correlation_id);
esp_err_t wifi_capture_store_freeze(void);
esp_err_t wifi_capture_store_unfreeze(bool reset_active_bank);
bool wifi_capture_store_get_snapshot(wifi_capture_store_snapshot_t *snapshot);
uint32_t wifi_capture_store_count(void);
uint32_t wifi_capture_store_overflow_count(void);
esp_err_t wifi_capture_store_save_async(void);
wifi_capture_save_status_t wifi_capture_store_save_request(void);
bool wifi_capture_store_is_saving(void);
bool wifi_capture_store_take_save_result(esp_err_t *result, char *path,
                                         uint32_t path_size);
