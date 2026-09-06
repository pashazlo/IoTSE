#include "storage.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"

static const char *TAG = "storage";

// Единый namespace для всех настроек — см. комментарий в storage.h
#define NVS_NAMESPACE   "app_cfg"

// Состояние FAT-раздела — нужно, чтобы не смонтировать его дважды
// и чтобы storage_fat_is_mounted() могла ответить без похода в VFS.
static bool s_fat_mounted = false;
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;


// ============================================================================
// NVS init
// ============================================================================

esp_err_t storage_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();

    // Обе эти ошибки означают "раздел есть, но с ним что-то не так":
    // либо физически повреждён (флеш кончает жизнь / был сбой питания
    // во время записи), либо структура версий NVS несовместима
    // (например, после смены версии ESP-IDF). В обоих случаях
    // единственный разумный выход — стереть раздел и начать заново.
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS раздел повреждён либо несовместим — стираем и переинициализируем");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NVS инициализирован");
    } else {
        ESP_LOGE(TAG, "Ошибка инициализации NVS: %s", esp_err_to_name(err));
    }

    return err;
}


// ============================================================================
// FAT init
// ============================================================================

esp_err_t storage_fat_init(void)
{
    if (s_fat_mounted) {
        // Не считаем это ошибкой — просто "уже сделано", ESP_OK.
        ESP_LOGW(TAG, "FAT-раздел уже смонтирован, повторный вызов игнорируется");
        return ESP_OK;
    }

    esp_vfs_fat_mount_config_t mount_config = {
        .max_files = 4,
        // true — только на случай самого первого запуска устройства,
        // когда раздел ещё пустой/неотформатирован. На всех
        // последующих запусках раздел уже валиден и не форматируется.
        .format_if_mount_failed = true,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };

    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(
        STORAGE_FAT_MOUNT_POINT,
        "storage",          // ИМЯ РАЗДЕЛА — должно совпадать со строкой в partitions.csv
        &mount_config,
        &s_wl_handle
    );

    if (err == ESP_OK) {
        s_fat_mounted = true;
        ESP_LOGI(TAG, "FAT-раздел смонтирован на %s", STORAGE_FAT_MOUNT_POINT);
    } else {
        ESP_LOGE(TAG, "Ошибка монтирования FAT-раздела: %s", esp_err_to_name(err));
    }

    return err;
}


bool storage_fat_is_mounted(void)
{
    return s_fat_mounted;
}


// ============================================================================
// NVS: u8
// ============================================================================

esp_err_t storage_nvs_get_u8(const char *key, uint8_t *out_value, uint8_t default_value)
{
    if (out_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Заполняем значением по умолчанию СРАЗУ — если что-то ниже пойдёт
    // не так (namespace ещё не создан, ключ не найден и т.п.),
    // *out_value всё равно останется корректным и безопасным для
    // использования, а не мусором со стека.
    *out_value = default_value;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        // Типичная причина на самом первом запуске: namespace ещё
        // ни разу не создавался (создаётся только при первой ЗАПИСИ).
        // Это не ошибка в прикладном смысле — просто "настроек ещё нет".
        return err;
    }

    err = nvs_get_u8(handle, key, out_value);
    nvs_close(handle);

    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_u8('%s') ошибка: %s", key, esp_err_to_name(err));
    }

    return err;
}


esp_err_t storage_nvs_set_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open (RW) ошибка при записи '%s': %s", key, esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(handle, key, value);
    if (err == ESP_OK) {
        // Без commit() запись остаётся только в RAM-кэше NVS и
        // потеряется при следующей перезагрузке/сбое питания.
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_u8('%s') ошибка: %s", key, esp_err_to_name(err));
    }

    return err;
}


// ============================================================================
// NVS: строки
// ============================================================================

esp_err_t storage_nvs_get_str(const char *key, char *out_buf, size_t buf_size)
{
    if (out_buf == NULL || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Как и с u8 — гарантируем безопасное состояние буфера сразу,
    // до любых попыток чтения из NVS.
    out_buf[0] = '\0';

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t required_size = buf_size;
    err = nvs_get_str(handle, key, out_buf, &required_size);
    nvs_close(handle);

    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        // В т.ч. сюда попадёт ESP_ERR_NVS_INVALID_LENGTH, если
        // buf_size меньше реально сохранённой строки — стоит смотреть
        // в лог при отладке, если строки внезапно обрезаются.
        ESP_LOGW(TAG, "nvs_get_str('%s') ошибка: %s", key, esp_err_to_name(err));
    }

    return err;
}


esp_err_t storage_nvs_set_str(const char *key, const char *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open (RW) ошибка при записи '%s': %s", key, esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str('%s') ошибка: %s", key, esp_err_to_name(err));
    }

    return err;
}
