#include "wifi_eapol_tracker.h"
#include "wifi_eapol_tracker_internal.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static wifi_eapol_tracker_state_t *s_state;
static SemaphoreHandle_t s_mutex;

esp_err_t wifi_eapol_tracker_init(void)
{
    if (s_state != NULL && s_mutex != NULL) return ESP_OK;
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    s_state = heap_caps_malloc(sizeof(*s_state),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_state == NULL) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    wifi_eapol_tracker_core_init(s_state);
    ESP_LOGI("WIFI_EAPOL", "PSRAM tracker: %u sessions, %u bytes",
             WIFI_EAPOL_TRACKER_CAPACITY, (unsigned)sizeof(*s_state));
    return ESP_OK;
}

void wifi_eapol_tracker_reset(void)
{
    if (s_state != NULL && s_mutex != NULL &&
        xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        wifi_eapol_tracker_core_reset(s_state);
        xSemaphoreGive(s_mutex);
    }
}

bool wifi_eapol_tracker_observe(const wifi_eapol_observation_t *observation)
{
    if (s_state == NULL || s_mutex == NULL || observation == NULL ||
        xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return false;
    bool result = wifi_eapol_tracker_core_observe(s_state, observation);
    xSemaphoreGive(s_mutex);
    return result;
}

bool wifi_eapol_tracker_get_snapshot(wifi_eapol_tracker_snapshot_t *snapshot)
{
    if (snapshot == NULL || s_state == NULL || s_mutex == NULL ||
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return false;
    wifi_eapol_tracker_core_snapshot(s_state, snapshot);
    xSemaphoreGive(s_mutex);
    return true;
}

bool wifi_eapol_tracker_get_diagnostics(
    wifi_eapol_tracker_diagnostics_t *diagnostics)
{
    if (diagnostics == NULL || s_state == NULL || s_mutex == NULL ||
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return false;
    *diagnostics = s_state->diagnostics;
    xSemaphoreGive(s_mutex);
    return true;
}

bool wifi_eapol_tracker_get_completed_count(uint16_t *completed_count)
{
    if (completed_count == NULL || s_state == NULL || s_mutex == NULL ||
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) != pdTRUE) return false;
    uint16_t count = 0;
    for (unsigned i = 0; i < WIFI_EAPOL_TRACKER_CAPACITY; ++i) {
        if (s_state->slots[i].occupied &&
            s_state->slots[i].session.state == WIFI_EAPOL_SESSION_COMPLETE)
            ++count;
    }
    *completed_count = count;
    xSemaphoreGive(s_mutex);
    return true;
}

size_t wifi_eapol_tracker_state_size(void) { return sizeof(*s_state); }
size_t wifi_eapol_tracker_snapshot_size(void)
{
    return sizeof(wifi_eapol_tracker_snapshot_t);
}
