#include "wifi_sta_tracker.h"
#include "wifi_sta_tracker_internal.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static wifi_sta_tracker_state_t *s_state; static SemaphoreHandle_t s_mutex;
esp_err_t wifi_sta_tracker_init(void){if(s_state&&s_mutex)return ESP_OK;s_mutex=xSemaphoreCreateMutex();if(!s_mutex)return ESP_ERR_NO_MEM;s_state=heap_caps_malloc(sizeof(*s_state),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);if(!s_state){vSemaphoreDelete(s_mutex);s_mutex=NULL;return ESP_ERR_NO_MEM;}wifi_sta_tracker_core_init(s_state);ESP_LOGI("WIFI_STA_TRACKER","PSRAM database: %u STA, %u bytes",WIFI_STA_TRACKER_CAPACITY,(unsigned)sizeof(*s_state));return ESP_OK;}
void wifi_sta_tracker_reset(void){if(s_state&&s_mutex&&xSemaphoreTake(s_mutex,portMAX_DELAY)==pdTRUE){wifi_sta_tracker_core_reset(s_state);xSemaphoreGive(s_mutex);}}
bool wifi_sta_tracker_observe(const wifi_sta_observation_t *o){if(!s_state||!s_mutex||!o)return false;if(xSemaphoreTake(s_mutex,portMAX_DELAY)!=pdTRUE)return false;bool ok=wifi_sta_tracker_core_observe(s_state,o);xSemaphoreGive(s_mutex);return ok;}
bool wifi_sta_tracker_get_snapshot(wifi_sta_tracker_snapshot_t *o){if(!o||!s_state||!s_mutex)return false;if(xSemaphoreTake(s_mutex,pdMS_TO_TICKS(10))!=pdTRUE)return false;wifi_sta_tracker_core_snapshot(s_state,o);xSemaphoreGive(s_mutex);return true;}
bool wifi_sta_tracker_get_diagnostics(wifi_sta_tracker_diagnostics_t *o){if(!o||!s_state||!s_mutex)return false;if(xSemaphoreTake(s_mutex,pdMS_TO_TICKS(10))!=pdTRUE)return false;*o=s_state->diagnostics;xSemaphoreGive(s_mutex);return true;}
