#include "wifi_capture_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_frame_parser.h"
#include "wifi_pcap_codec.h"
#include "file_service.h"

#define CAPTURE_PATH_SIZE STORAGE_MANAGER_PATH_MAX
#define RECORD_FLAG_EAPOL 0x01U
static const char *TAG = "WIFI_PCAP";

static esp_err_t file_error(void)
{
    return errno == ENOSPC ? ESP_ERR_NO_MEM : ESP_FAIL;
}

typedef struct __attribute__((packed)) {
    uint32_t stored_size;
    uint64_t sequence;
    int64_t timestamp_us;
    uint32_t correlation_id;
    uint16_t original_length;
    uint16_t captured_length;
    int8_t rssi;
    uint8_t channel;
    uint8_t packet_type;
    uint8_t frame_type;
    uint8_t flags;
    uint8_t legacy_rate;
    uint8_t sig_mode;
    uint8_t mcs;
    uint8_t bandwidth_40mhz;
    uint8_t short_gi;
    uint8_t target_diag;
} capture_record_t;

_Static_assert(sizeof(capture_record_t) == 39U,
               "Unexpected capture ring record header size");

typedef struct {
    uint8_t *data;
    uint32_t head;
    uint32_t tail;
    uint32_t used;
    uint32_t count;
} capture_bank_t;

static capture_bank_t s_banks[WIFI_CAPTURE_BANK_COUNT];
static uint8_t s_active_bank;
static int8_t s_sealed_bank = -1;
static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_save_queue;
static TaskHandle_t s_task;
static bool s_saving;
static bool s_frozen;
static bool s_result_ready;
static esp_err_t s_save_result;
static char s_saved_path[CAPTURE_PATH_SIZE];
static uint32_t s_correlation_id;
static uint64_t s_next_sequence = 1U;
static uint32_t s_high_water_bytes;
static uint32_t s_high_water_records;
static uint32_t s_overwritten_records;
static uint32_t s_overwritten_bytes;
static atomic_uint_fast32_t s_dropped_busy;
static uint32_t s_dropped_frozen;
static uint32_t s_dropped_oversize;
static uint32_t s_corruption_resets;
static uint32_t s_save_busy_count;

static uint32_t align4(uint32_t value)
{
    return (value + 3U) & ~3U;
}

static void bank_reset(capture_bank_t *bank)
{
    bank->head = bank->tail = bank->used = bank->count = 0U;
}

static void ring_copy_in(capture_bank_t *bank, uint32_t offset,
                         const void *source, uint32_t length)
{
    uint32_t first = WIFI_CAPTURE_BANK_BYTES - offset;
    if (first > length) first = length;
    memcpy(bank->data + offset, source, first);
    if (length > first) memcpy(bank->data, (const uint8_t *)source + first,
                               length - first);
}

static void ring_copy_out(const capture_bank_t *bank, uint32_t offset,
                          void *destination, uint32_t length)
{
    uint32_t first = WIFI_CAPTURE_BANK_BYTES - offset;
    if (first > length) first = length;
    memcpy(destination, bank->data + offset, first);
    if (length > first) memcpy((uint8_t *)destination + first, bank->data,
                               length - first);
}

static bool record_size_valid(uint32_t size)
{
    return size >= align4(sizeof(capture_record_t)) &&
           size <= WIFI_CAPTURE_BANK_BYTES && (size & 3U) == 0U;
}

static void recover_corrupt_bank(capture_bank_t *bank)
{
    bank_reset(bank);
    ++s_corruption_resets;
}

static bool evict_one(capture_bank_t *bank)
{
    capture_record_t record;
    if (bank->count == 0U || bank->used < sizeof(record)) return false;
    ring_copy_out(bank, bank->tail, &record, sizeof(record));
    if (!record_size_valid(record.stored_size) || record.stored_size > bank->used) {
        recover_corrupt_bank(bank);
        return false;
    }
    bank->tail = (bank->tail + record.stored_size) % WIFI_CAPTURE_BANK_BYTES;
    bank->used -= record.stored_size;
    --bank->count;
    ++s_overwritten_records;
    s_overwritten_bytes += record.stored_size;
    return true;
}

static void cleanup_partial_init(void)
{
    if (s_save_queue != NULL) vQueueDelete(s_save_queue);
    if (s_mutex != NULL) vSemaphoreDelete(s_mutex);
    for (size_t i = 0; i < WIFI_CAPTURE_BANK_COUNT; ++i) {
        if (s_banks[i].data != NULL) heap_caps_free(s_banks[i].data);
    }
    memset(s_banks, 0, sizeof(s_banks));
    s_save_queue = NULL;
    s_mutex = NULL;
    s_task = NULL;
    s_active_bank = 0U;
    s_sealed_bank = -1;
    s_saving = s_frozen = s_result_ready = false;
    s_save_result = ESP_OK;
    s_saved_path[0] = '\0';
    s_correlation_id = 0U;
    s_next_sequence = 1U;
    s_high_water_bytes = s_high_water_records = 0U;
    s_overwritten_records = s_overwritten_bytes = 0U;
    atomic_store_explicit(&s_dropped_busy, 0U, memory_order_release);
    s_dropped_frozen = s_dropped_oversize = s_corruption_resets = 0U;
    s_save_busy_count = 0U;
}

static esp_err_t next_path(char out[CAPTURE_PATH_SIZE])
{
    esp_err_t err = file_service_make_unique(STORAGE_ROLE_CAPTURE_WIFI_PCAP,
                                             "wifi", ".pcap", out,
                                             CAPTURE_PATH_SIZE);
    if (err == ESP_ERR_NOT_FOUND)
        ESP_LOGE(TAG, "Save unavailable: SD card is not mounted");
    return err;
}

static wifi_pcap_phy_t record_phy(const capture_record_t *record)
{
    if (record->sig_mode == 0U &&
        wifi_pcap_legacy_rate_500kbps(record->legacy_rate) != 0U) {
        return WIFI_PCAP_PHY_LEGACY;
    }
    if (record->sig_mode == 1U) return WIFI_PCAP_PHY_HT;
    return WIFI_PCAP_PHY_UNKNOWN;
}

static bool write_ring_bytes(FILE *file, const capture_bank_t *bank,
                             uint32_t offset, uint32_t length)
{
    uint32_t first = WIFI_CAPTURE_BANK_BYTES - offset;
    if (first > length) first = length;
    if (first != 0U && fwrite(bank->data + offset, 1, first, file) != first) {
        return false;
    }
    uint32_t second = length - first;
    return second == 0U || fwrite(bank->data, 1, second, file) == second;
}

static esp_err_t write_capture(uint8_t bank_index, char path[CAPTURE_PATH_SIZE])
{
    file_service_file_t output;
    esp_err_t err = ESP_FAIL;
    /* "x" requests exclusive creation. If another writer creates the chosen
     * name after the directory scan, rescan and advance instead of truncating. */
    for (unsigned attempt = 0; attempt < 8U; ++attempt) {
        err = next_path(path);
        if (err != ESP_OK) return err;
        err = file_service_open(path, "wbx", &output);
        if (err == ESP_OK) break;
        if (err != ESP_ERR_INVALID_STATE) return err;
    }
    if (err != ESP_OK) return ESP_ERR_INVALID_STATE;
    FILE *file = output.stream;

    uint8_t global[WIFI_PCAP_GLOBAL_HEADER_LEN];
    if (wifi_pcap_write_global_header(global, sizeof(global),
                                      WIFI_PCAP_FILE_SNAPLEN) != sizeof(global) ||
        fwrite(global, 1, sizeof(global), file) != sizeof(global)) {
        err = ESP_FAIL;
    }

    const capture_bank_t *bank = &s_banks[bank_index];
    uint32_t offset = bank->tail;
    uint32_t exported_ap = 0U;
    uint32_t exported_sta = 0U;
    for (uint32_t i = 0; err == ESP_OK && i < bank->count; ++i) {
        capture_record_t record;
        ring_copy_out(bank, offset, &record, sizeof(record));
        if (!record_size_valid(record.stored_size) ||
            record.captured_length > record.stored_size - sizeof(record)) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        wifi_pcap_radio_meta_t meta = {
            .phy = record_phy(&record),
            .legacy_rate_code = record.legacy_rate,
            .mcs = record.mcs,
            .bandwidth_40mhz = record.bandwidth_40mhz != 0U,
            .short_gi = record.short_gi != 0U,
            .rssi_dbm = record.rssi,
            .channel = record.channel,
        };
        uint8_t radiotap[WIFI_RADIOTAP_MAX_LEN];
        size_t radiotap_len = wifi_radiotap_write(
            radiotap, sizeof(radiotap), &meta);
        if (radiotap_len == 0U) { err = ESP_ERR_INVALID_ARG; break; }

        uint16_t selected = wifi_pcap_select_frame_length(
            record.frame_type,
            (record.flags & RECORD_FLAG_EAPOL) != 0U,
            record.captured_length);
        uint32_t original_frame = record.original_length;
        uint8_t packet_header[WIFI_PCAP_RECORD_HEADER_LEN];
        uint32_t seconds = (uint32_t)(record.timestamp_us / 1000000LL);
        uint32_t micros = (uint32_t)(record.timestamp_us % 1000000LL);
        if (wifi_pcap_write_record_header(
                packet_header, sizeof(packet_header), seconds, micros,
                (uint32_t)radiotap_len + selected,
                (uint32_t)radiotap_len + original_frame) == 0U ||
            fwrite(packet_header, 1, sizeof(packet_header), file) !=
                sizeof(packet_header) ||
            fwrite(radiotap, 1, radiotap_len, file) != radiotap_len ||
            !write_ring_bytes(file, bank,
                              (offset + sizeof(record)) % WIFI_CAPTURE_BANK_BYTES,
                              selected)) {
            err = file_error();
            break;
        }
        if (record.target_diag == WIFI_TARGET_DIAG_AP) ++exported_ap;
        else if (record.target_diag == WIFI_TARGET_DIAG_STA) ++exported_sta;
        offset = (offset + record.stored_size) % WIFI_CAPTURE_BANK_BYTES;
    }
    if (err == ESP_OK) err = file_service_flush_sync(&output);
    esp_err_t close_err = file_service_close(&output);
    if (err == ESP_OK) err = close_err;
    if (err == ESP_OK) wifi_target_diag_note_export(exported_ap, exported_sta);
    if (err != ESP_OK) (void)file_service_remove(path);
    return err;
}

static void writer_task(void *unused)
{
    (void)unused;
    uint8_t bank_index;
    while (true) {
        if (xQueueReceive(s_save_queue, &bank_index, portMAX_DELAY) != pdTRUE) continue;
        char path[CAPTURE_PATH_SIZE] = "";
        esp_err_t result = write_capture(bank_index, path);
        if (xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
            if (result == ESP_OK) {
                bank_reset(&s_banks[bank_index]);
                if (s_sealed_bank == (int8_t)bank_index) s_sealed_bank = -1;
            } else {
                ESP_LOGE(TAG, "Writer failed: %s; sealed bank %u retained",
                         esp_err_to_name(result), bank_index);
            }
            s_save_result = result;
            snprintf(s_saved_path, sizeof(s_saved_path), "%s", path);
            s_saving = false;
            s_result_ready = true;
            xSemaphoreGive(s_mutex);
        }
    }
}

esp_err_t wifi_capture_store_init(void)
{
    if (s_task != NULL) return ESP_OK;
    cleanup_partial_init();
    for (size_t i = 0; i < WIFI_CAPTURE_BANK_COUNT; ++i) {
        s_banks[i].data = heap_caps_malloc(
            WIFI_CAPTURE_BANK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_banks[i].data == NULL) {
            cleanup_partial_init();
            return ESP_ERR_NO_MEM;
        }
    }
    s_mutex = xSemaphoreCreateMutex();
    s_save_queue = xQueueCreate(1, sizeof(uint8_t));
    if (s_mutex == NULL || s_save_queue == NULL ||
        xTaskCreate(writer_task, "wifi_pcap", 4096, NULL, 2, &s_task) != pdPASS) {
        cleanup_partial_init();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void wifi_capture_store_reset(void)
{
    if (s_mutex != NULL && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        bank_reset(&s_banks[s_active_bank]);
        s_high_water_bytes = s_high_water_records = 0U;
        s_overwritten_records = s_overwritten_bytes = 0U;
        atomic_store_explicit(&s_dropped_busy, 0U, memory_order_release);
        s_dropped_frozen = s_dropped_oversize = 0U;
        s_corruption_resets = 0U;
        s_save_busy_count = 0U;
        xSemaphoreGive(s_mutex);
    }
}

bool wifi_capture_store_append(const wifi_raw_packet_t *packet)
{
    if (packet == NULL || s_mutex == NULL) return false;
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        atomic_fetch_add_explicit(&s_dropped_busy, 1U, memory_order_relaxed);
        return false;
    }
    if (s_frozen) {
        ++s_dropped_frozen;
        xSemaphoreGive(s_mutex);
        return false;
    }
    uint32_t stored_size = align4((uint32_t)sizeof(capture_record_t) +
                                  packet->captured_length);
    if (packet->captured_length > WIFI_PROMISCUOUS_CAPTURE_MAX_LEN ||
        stored_size > WIFI_CAPTURE_BANK_BYTES) {
        ++s_dropped_oversize;
        xSemaphoreGive(s_mutex);
        return false;
    }

    capture_bank_t *bank = &s_banks[s_active_bank];
    while ((bank->used + stored_size > WIFI_CAPTURE_BANK_BYTES ||
            bank->count >= WIFI_CAPTURE_MAX_RECORDS) && bank->count > 0U) {
        if (!evict_one(bank)) break;
    }
    if (bank->used + stored_size > WIFI_CAPTURE_BANK_BYTES ||
        bank->count >= WIFI_CAPTURE_MAX_RECORDS) {
        ++s_dropped_oversize;
        xSemaphoreGive(s_mutex);
        return false;
    }

    wifi_frame_info_t info;
    bool parsed = wifi_frame_parse(packet->payload, packet->captured_length,
                                   packet->rssi, packet->channel, &info);
    capture_record_t record = {
        .stored_size = stored_size,
        .sequence = s_next_sequence++,
        .timestamp_us = esp_timer_get_time(),
        .correlation_id = s_correlation_id,
        .original_length = packet->original_length,
        .captured_length = packet->captured_length,
        .rssi = packet->rssi,
        .channel = packet->channel,
        .packet_type = (uint8_t)packet->type,
        .frame_type = parsed ? info.type :
            (packet->type == WIFI_PKT_MGMT ? WIFI_FRAME_TYPE_MGMT :
             packet->type == WIFI_PKT_CTRL ? WIFI_FRAME_TYPE_CTRL :
                                             WIFI_FRAME_TYPE_DATA),
        .flags = parsed && info.is_eapol ? RECORD_FLAG_EAPOL : 0U,
        .legacy_rate = packet->legacy_rate,
        .sig_mode = packet->sig_mode,
        .mcs = packet->mcs,
        .bandwidth_40mhz = packet->bandwidth_40mhz ? 1U : 0U,
        .short_gi = packet->short_gi ? 1U : 0U,
        .target_diag = (uint8_t)packet->target_diag,
    };
    ring_copy_in(bank, bank->head, &record, sizeof(record));
    ring_copy_in(bank, (bank->head + sizeof(record)) % WIFI_CAPTURE_BANK_BYTES,
                 packet->payload, packet->captured_length);
    bank->head = (bank->head + stored_size) % WIFI_CAPTURE_BANK_BYTES;
    bank->used += stored_size;
    ++bank->count;
    if (bank->used > s_high_water_bytes) s_high_water_bytes = bank->used;
    if (bank->count > s_high_water_records) s_high_water_records = bank->count;
    xSemaphoreGive(s_mutex);
    return true;
}

void wifi_capture_store_set_correlation_id(uint32_t correlation_id)
{
    if (s_mutex != NULL && xSemaphoreTake(s_mutex, portMAX_DELAY) == pdTRUE) {
        s_correlation_id = correlation_id;
        xSemaphoreGive(s_mutex);
    }
}

esp_err_t wifi_capture_store_freeze(void)
{
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    s_frozen = true;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t wifi_capture_store_unfreeze(bool reset_active_bank)
{
    if (s_mutex == NULL || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (reset_active_bank) bank_reset(&s_banks[s_active_bank]);
    s_frozen = false;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

bool wifi_capture_store_get_snapshot(wifi_capture_store_snapshot_t *out)
{
    if (out == NULL || s_mutex == NULL || xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        return false;
    }
    const capture_bank_t *bank = &s_banks[s_active_bank];
    *out = (wifi_capture_store_snapshot_t) {
        .capacity_bytes = WIFI_CAPTURE_BANK_BYTES,
        .used_bytes = bank->used,
        .record_count = bank->count,
        .high_water_bytes = s_high_water_bytes,
        .high_water_records = s_high_water_records,
        .overwritten_records = s_overwritten_records,
        .overwritten_bytes = s_overwritten_bytes,
        .dropped_busy = (uint32_t)atomic_load_explicit(
            &s_dropped_busy, memory_order_acquire),
        .dropped_frozen = s_dropped_frozen,
        .dropped_oversize = s_dropped_oversize,
        .corruption_resets = s_corruption_resets,
        .save_busy_count = s_save_busy_count,
        .correlation_id = s_correlation_id,
        .next_sequence = s_next_sequence,
        .frozen = s_frozen,
        .saving = s_saving,
        .active_bank = s_active_bank,
        .sealed_bank = s_sealed_bank,
        .writer_stack_hwm = s_task != NULL
            ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) : 0U,
    };
    xSemaphoreGive(s_mutex);
    return true;
}

static uint32_t snapshot_overflow_total(void)
{
    wifi_capture_store_snapshot_t snapshot;
    return wifi_capture_store_get_snapshot(&snapshot)
        ? snapshot.overwritten_records + snapshot.dropped_busy +
          snapshot.dropped_frozen + snapshot.dropped_oversize
        : 0U;
}

uint32_t wifi_capture_store_count(void)
{
    wifi_capture_store_snapshot_t snapshot;
    return wifi_capture_store_get_snapshot(&snapshot) ? snapshot.record_count : 0U;
}

uint32_t wifi_capture_store_overflow_count(void)
{
    return snapshot_overflow_total();
}

esp_err_t wifi_capture_store_save_async(void)
{
    wifi_capture_save_status_t status = wifi_capture_store_save_request();
    if (status == WIFI_CAPTURE_SAVE_QUEUED) return ESP_OK;
    if (status == WIFI_CAPTURE_SAVE_BUSY) return ESP_ERR_NOT_FINISHED;
    if (status == WIFI_CAPTURE_SAVE_EMPTY) return ESP_ERR_INVALID_SIZE;
    return ESP_FAIL;
}

wifi_capture_save_status_t wifi_capture_store_save_request(void)
{
    if (s_mutex == NULL || s_save_queue == NULL)
        return WIFI_CAPTURE_SAVE_ERROR;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE)
        return WIFI_CAPTURE_SAVE_ERROR;
    capture_bank_t *active = &s_banks[s_active_bank];
    if (s_saving) {
        ++s_save_busy_count;
        xSemaphoreGive(s_mutex);
        return WIFI_CAPTURE_SAVE_BUSY;
    }
    if (s_sealed_bank >= 0) {
        uint8_t retry = (uint8_t)s_sealed_bank;
        s_saving = true;
        s_result_ready = false;
        if (xQueueSend(s_save_queue, &retry, 0) != pdTRUE) {
            s_saving = false;
            xSemaphoreGive(s_mutex);
            return WIFI_CAPTURE_SAVE_ERROR;
        }
        xSemaphoreGive(s_mutex);
        return WIFI_CAPTURE_SAVE_QUEUED;
    }
    if (active->count == 0U) {
        xSemaphoreGive(s_mutex);
        return WIFI_CAPTURE_SAVE_EMPTY;
    }

    uint8_t sealed = s_active_bank;
    uint8_t next = (uint8_t)((s_active_bank + 1U) % WIFI_CAPTURE_BANK_COUNT);
    bank_reset(&s_banks[next]);
    s_active_bank = next;
    s_sealed_bank = (int8_t)sealed;
    s_saving = true;
    s_result_ready = false;
    if (xQueueSend(s_save_queue, &sealed, 0) != pdTRUE) {
        s_active_bank = sealed;
        s_sealed_bank = -1;
        s_saving = false;
        bank_reset(&s_banks[next]);
        xSemaphoreGive(s_mutex);
        return WIFI_CAPTURE_SAVE_ERROR;
    }
    xSemaphoreGive(s_mutex);
    return WIFI_CAPTURE_SAVE_QUEUED;
}

bool wifi_capture_store_is_saving(void)
{
    wifi_capture_store_snapshot_t snapshot;
    return wifi_capture_store_get_snapshot(&snapshot) && snapshot.saving;
}

bool wifi_capture_store_take_save_result(esp_err_t *result, char *path,
                                         uint32_t path_size)
{
    if (result == NULL || path == NULL || path_size == 0U || s_mutex == NULL ||
        xSemaphoreTake(s_mutex, 0) != pdTRUE) return false;
    if (!s_result_ready) {
        xSemaphoreGive(s_mutex);
        return false;
    }
    *result = s_save_result;
    snprintf(path, path_size, "%s", s_saved_path);
    s_result_ready = false;
    xSemaphoreGive(s_mutex);
    return true;
}
