#include "wifi_pmkid_store.h"
#include "wifi_pmkid_store_internal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Публичная обёртка хранилища PMKID: единственный экземпляр состояния + mutex.
 * Вся логика — в core (wifi_pmkid_store_core.c), здесь только владение и защита
 * доступа из двух задач (анализатор пишет, UI читает).
 *
 * Состояние держим статикой (~1.8 КБ): оно небольшое и живёт всё время работы,
 * поэтому PSRAM-аллокация (как у крупного eapol-трекера) здесь избыточна. */
static wifi_pmkid_store_state_t s_state;
static SemaphoreHandle_t s_mutex;

esp_err_t wifi_pmkid_store_init(void)
{
    if (s_mutex != NULL) {
        return ESP_OK; /* идемпотентно */
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    wifi_pmkid_store_core_reset(&s_state);
    return ESP_OK;
}

void wifi_pmkid_store_reset(void)
{
    if (s_mutex != NULL &&
        xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        wifi_pmkid_store_core_reset(&s_state);
        xSemaphoreGive(s_mutex);
    }
}

bool wifi_pmkid_store_observe(const wifi_pmkid_record_t *rec)
{
    if (s_mutex == NULL || rec == NULL) {
        return false;
    }
    /* portMAX_DELAY безопасен: mutex удерживается лишь на короткое копирование. */
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    bool added = wifi_pmkid_store_core_observe(&s_state, rec);
    xSemaphoreGive(s_mutex);
    return added;
}

uint16_t wifi_pmkid_store_count(void)
{
    if (s_mutex == NULL) {
        return 0U;
    }
    uint16_t n = 0U;
    /* UI не должен залипать: короткий таймаут, при неудаче вернём 0. */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        n = s_state.count;
        xSemaphoreGive(s_mutex);
    }
    return n;
}

bool wifi_pmkid_store_get_snapshot(wifi_pmkid_store_snapshot_t *out)
{
    if (out == NULL || s_mutex == NULL) {
        return false;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return false;
    }
    wifi_pmkid_store_core_snapshot(&s_state, out);
    xSemaphoreGive(s_mutex);
    return true;
}
