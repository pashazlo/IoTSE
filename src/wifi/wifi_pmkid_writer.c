#include "wifi_pmkid_writer.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "file_service.h"      /* resolve/open/close + STORAGE_MANAGER_PATH_MAX, роли */
#include "storage_manager.h"   /* create_layout — гарантировать каталоги на SD */
#include "wifi_ap_tracker.h"   /* резолв SSID по BSSID */

static const char *TAG = "PMKID_WR";

/* Параметры задачи — как у PCAP-писателя (фоновая запись): стек 4096, приоритет 2,
 * очередь небольшой глубины (находки редки, но несколько подряд не потеряем). */
#define WRITER_QUEUE_LEN   8U
#define WRITER_TASK_STACK  4096
#define WRITER_TASK_PRIO   2

/* Фиксированное имя файла в каталоге роли «handshakes» на SD. Дозаписываем в него. */
#define PMKID_FILE_LEAF    "pmkid.hc22000"

static QueueHandle_t s_queue;
static TaskHandle_t  s_task;

/* Результат последней записи — для статуса в UI. Пишет задача, читает UI → atomics. */
static atomic_int  s_last_result;
static atomic_bool s_result_pending;

/* Снимок ap_tracker для резолва SSID. Большой (~20 КБ) — держим в PSRAM и
 * переиспользуем (находки редки, снимать на каждую запись допустимо). */
static wifi_ap_tracker_snapshot_t *s_ap_snapshot;

/* hex-кодирование: n байт → 2n символов + NUL. out должен вмещать 2n+1. */
static void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        out[2U * i]      = digits[(in[i] >> 4) & 0x0FU];
        out[2U * i + 1U] = digits[in[i] & 0x0FU];
    }
    out[2U * n] = '\0';
}

/* Найти SSID сети по её BSSID в снимке ap_tracker. Пусто, если не нашли или снимок
 * недоступен — запись всё равно сохраним (PMKID+MAC не теряем), но ESSID будет пуст. */
static void resolve_ssid(const uint8_t bssid[6], char *out, size_t out_size)
{
    out[0] = '\0';
    if (s_ap_snapshot == NULL) {
        return;
    }
    if (!wifi_ap_tracker_get_snapshot(s_ap_snapshot)) {
        return;
    }
    for (uint16_t i = 0; i < s_ap_snapshot->count; ++i) {
        if (memcmp(s_ap_snapshot->records[i].bssid, bssid, 6) == 0) {
            snprintf(out, out_size, "%s", s_ap_snapshot->records[i].ssid);
            return;
        }
    }
}

/* Собрать строку hashcat 22000 и дописать её в файл на SD. */
static esp_err_t write_record(const wifi_pmkid_record_t *rec)
{
    /* ESSID: если запись уже несёт SSID — берём его, иначе резолвим по BSSID. */
    char ssid[WIFI_PMKID_SSID_MAX + 1];
    if (rec->ssid[0] != '\0') {
        snprintf(ssid, sizeof(ssid), "%s", rec->ssid);
    } else {
        resolve_ssid(rec->bssid, ssid, sizeof(ssid));
    }

    char pmkid_hex[2U * WIFI_PMKID_LEN + 1U];
    char ap_hex[2U * 6U + 1U];
    char sta_hex[2U * 6U + 1U];
    char essid_hex[2U * WIFI_PMKID_SSID_MAX + 1U];
    hex_encode(rec->pmkid, WIFI_PMKID_LEN, pmkid_hex);
    hex_encode(rec->bssid, 6, ap_hex);
    hex_encode(rec->sta, 6, sta_hex);
    hex_encode((const uint8_t *)ssid, strnlen(ssid, WIFI_PMKID_SSID_MAX), essid_hex);

    /* Гарантируем, что каталоги на SD существуют. storage_manager_create_layout()
     * идемпотентен (каждый каталог проверяется stat перед mkdir) и создаёт всё
     * дерево /sdcard/Captures/WiFi/Handshakes, если SD смонтирована. Делаем это
     * здесь, а не только на бусте: карту могли вставить позже, и тогда папки ещё
     * нет. Находки PMKID редки — несколько stat на запись дёшевы. Результат
     * best-effort: если SD не смонтирована, layout ничего не создаст, а
     * последующий resolve/open честно вернёт ошибку ниже. */
    (void)storage_manager_create_layout();

    /* Путь на SD (роль handshakes). NOT_FOUND = карта не смонтирована → не пишем. */
    char path[STORAGE_MANAGER_PATH_MAX];
    esp_err_t err = file_service_resolve(STORAGE_ROLE_CAPTURE_WIFI_HANDSHAKES,
                                         PMKID_FILE_LEAF, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }

    /* Дозапись ("ab"): файл создаётся при первом разе, далее строки добавляются.
     * Открываем-пишем-закрываем на каждую находку (они редки) — так файл всегда
     * согласован и переживает внезапное выключение. */
    file_service_file_t file;
    err = file_service_open(path, "ab", &file);
    if (err != ESP_OK) {
        return err;
    }
    int written = fprintf(file.stream, "WPA*01*%s*%s*%s*%s***\n",
                          pmkid_hex, ap_hex, sta_hex, essid_hex);
    esp_err_t write_err = (written > 0) ? ESP_OK : ESP_FAIL;
    esp_err_t close_err = file_service_close(&file);
    return (write_err != ESP_OK) ? write_err : close_err;
}

static void writer_task(void *arg)
{
    (void)arg;
    wifi_pmkid_record_t rec;
    for (;;) {
        /* portMAX_DELAY допустим: это выделенная задача-писатель. */
        if (xQueueReceive(s_queue, &rec, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        esp_err_t err = write_record(&rec);
        atomic_store_explicit(&s_last_result, (int)err, memory_order_relaxed);
        atomic_store_explicit(&s_result_pending, true, memory_order_release);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "PMKID saved to SD");
        } else if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "PMKID not saved: SD not mounted");
        } else {
            ESP_LOGW(TAG, "PMKID write failed: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t wifi_pmkid_writer_init(void)
{
    if (s_task != NULL) {
        return ESP_OK; /* идемпотентно */
    }
    s_queue = xQueueCreate(WRITER_QUEUE_LEN, sizeof(wifi_pmkid_record_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* Снимок ap_tracker в PSRAM. Если не выделился — не фатально: ESSID будет
     * пустым, но PMKID+MAC всё равно сохраним. */
    s_ap_snapshot = heap_caps_malloc(sizeof(*s_ap_snapshot),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ap_snapshot == NULL) {
        ESP_LOGW(TAG, "no PSRAM for AP snapshot: ESSID may be empty");
    }
    if (xTaskCreate(writer_task, "wifi_pmkid_wr", WRITER_TASK_STACK, NULL,
                    WRITER_TASK_PRIO, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        if (s_ap_snapshot != NULL) {
            heap_caps_free(s_ap_snapshot);
            s_ap_snapshot = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    atomic_store_explicit(&s_result_pending, false, memory_order_relaxed);
    return ESP_OK;
}

bool wifi_pmkid_writer_submit(const wifi_pmkid_record_t *rec)
{
    if (s_queue == NULL || rec == NULL) {
        return false;
    }
    return xQueueSend(s_queue, rec, 0) == pdTRUE;
}

bool wifi_pmkid_writer_take_result(esp_err_t *result)
{
    if (result == NULL) {
        return false;
    }
    if (!atomic_exchange_explicit(&s_result_pending, false, memory_order_acquire)) {
        return false; /* новых результатов не было */
    }
    *result = (esp_err_t)atomic_load_explicit(&s_last_result, memory_order_relaxed);
    return true;
}
