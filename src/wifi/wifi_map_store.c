#include "wifi_map_store.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define WIFI_MAP_STORE_LOCK_TIMEOUT_MS 10U

static const char *TAG = "WIFI_MAP_STORE";

static wifi_map_snapshot_t *s_store;
static SemaphoreHandle_t s_store_mutex;

esp_err_t wifi_map_store_init(void)
{
    if (s_store != NULL && s_store_mutex != NULL) return ESP_OK;

    if (s_store_mutex == NULL) {
        s_store_mutex = xSemaphoreCreateMutex();
        if (s_store_mutex == NULL) return ESP_ERR_NO_MEM;
    }

    s_store = heap_caps_calloc(
        1,
        sizeof(*s_store),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (s_store == NULL) {
        vSemaphoreDelete(s_store_mutex);
        s_store_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_store->format_version = WIFI_MAP_STORE_FORMAT_VERSION;
    ESP_LOGI(TAG, "PSRAM store allocated: %u bytes",
             (unsigned)sizeof(*s_store));
    return ESP_OK;
}

bool wifi_map_store_is_ready(void)
{
    return s_store != NULL && s_store_mutex != NULL;
}

size_t wifi_map_store_size(void)
{
    return sizeof(wifi_map_snapshot_t);
}

esp_err_t wifi_map_store_publish_port_scan(
    const wifi_port_scan_result_t *result,
    uint32_t *generation
)
{
    if (result == NULL) return ESP_ERR_INVALID_ARG;
    if (!wifi_map_store_is_ready()) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_store_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint32_t next_generation = s_store->generation + 1U;
    if (next_generation == 0U) next_generation = 1U;
    memcpy(&s_store->port_scan, result, sizeof(*result));
    s_store->format_version = WIFI_MAP_STORE_FORMAT_VERSION;
    s_store->generation = next_generation;
    xSemaphoreGive(s_store_mutex);

    if (generation != NULL) *generation = next_generation;
    return ESP_OK;
}

bool wifi_map_store_get_snapshot(wifi_map_snapshot_t *snapshot)
{
    if (snapshot == NULL || !wifi_map_store_is_ready()) return false;
    if (xSemaphoreTake(
            s_store_mutex,
            pdMS_TO_TICKS(WIFI_MAP_STORE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }
    memcpy(snapshot, s_store, sizeof(*snapshot));
    xSemaphoreGive(s_store_mutex);
    return snapshot->generation != 0U;
}

bool wifi_map_store_get_port_scan(
    uint32_t expected_generation,
    wifi_port_scan_result_t *result
)
{
    if (result == NULL || expected_generation == 0U ||
        !wifi_map_store_is_ready()) {
        return false;
    }
    if (xSemaphoreTake(
            s_store_mutex,
            pdMS_TO_TICKS(WIFI_MAP_STORE_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return false;
    }

    bool matches = s_store->generation == expected_generation;
    if (matches) memcpy(result, &s_store->port_scan, sizeof(*result));
    xSemaphoreGive(s_store_mutex);
    return matches;
}
