#include "wifi_ap_tracker.h"

#include "wifi_ap_tracker_internal.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define WIFI_AP_TRACKER_LOCK_TIMEOUT_MS 10U

static const char *TAG = "WIFI_AP_TRACKER";
static wifi_ap_tracker_state_t *s_state;
static SemaphoreHandle_t s_mutex;

esp_err_t wifi_ap_tracker_init(void)
{
    if (s_state != NULL && s_mutex != NULL) return ESP_OK;
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    s_state = heap_caps_malloc(sizeof(*s_state), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_state == NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    wifi_ap_tracker_core_init(s_state);
    ESP_LOGI(TAG, "PSRAM database allocated: %u AP, %u bytes",
             WIFI_AP_TRACKER_CAPACITY, (unsigned)sizeof(*s_state));
    return ESP_OK;
}

void wifi_ap_tracker_reset(void)
{
    if (s_state == NULL || s_mutex == NULL) return;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        wifi_ap_tracker_core_reset(s_state);
        xSemaphoreGive(s_mutex);
    }
}

bool wifi_ap_tracker_observe(const wifi_ap_observation_t *observation)
{
    if (s_state == NULL || s_mutex == NULL || observation == NULL) return false;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return false;
    const bool accepted = wifi_ap_tracker_core_observe(s_state, observation);
    xSemaphoreGive(s_mutex);
    return accepted;
}

bool wifi_ap_tracker_get_snapshot(wifi_ap_tracker_snapshot_t *snapshot)
{
    if (snapshot == NULL || s_state == NULL || s_mutex == NULL) return false;
    if (xSemaphoreTake(s_mutex,
                       pdMS_TO_TICKS(WIFI_AP_TRACKER_LOCK_TIMEOUT_MS)) != pdTRUE)
        return false;
    wifi_ap_tracker_core_snapshot(s_state, snapshot);
    xSemaphoreGive(s_mutex);
    return true;
}
bool wifi_ap_tracker_get_diagnostics(wifi_ap_tracker_diagnostics_t *diagnostics)
{
    if (diagnostics == NULL || s_state == NULL || s_mutex == NULL) return false;
    if (xSemaphoreTake(s_mutex,
                       pdMS_TO_TICKS(WIFI_AP_TRACKER_LOCK_TIMEOUT_MS)) != pdTRUE)
        return false;
    *diagnostics = s_state->diagnostics;
    xSemaphoreGive(s_mutex);
    return true;
}
