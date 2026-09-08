#include "storage.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"

static const char *TAG = "storage";

// Единый namespace для всех настроек
#define NVS_NAMESPACE "app_cfg"

// Состояние FAT-раздела
static bool s_fat_mounted = false;
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

// ============================================================================
// NVS init
// ============================================================================

esp_err_t storage_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS раздел повреждён либо несовместим — стираем и переинициализируем");
        esp_err_t erase_err = nvs_flash_erase();
        if (erase_err != ESP_OK) {
            ESP_LOGE(TAG, "Ошибка стирания NVS: %s", esp_err_to_name(erase_err));
            return erase_err;
        }
        err = nvs_flash_init();
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NVS успешно инициализирован");
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
        ESP_LOGW(TAG, "FAT-раздел уже смонтирован, повторный вызов игнорируется");
        return ESP_OK;
    }

    esp_vfs_fat_mount_config_t mount_config = {
        .max_files = 4,
        .format_if_mount_failed = true,
        // 0 заставляет VFS автоматически выбрать безопасный размер кластера
        .allocation_unit_size = 0,
    };

    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(
        STORAGE_FAT_MOUNT_POINT,
        "storage",          // Название раздела должно быть "storage" в partitions.csv
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

esp_err_t storage_fat_deinit(void)
{
    if (!s_fat_mounted) {
        return ESP_OK;
    }

    esp_err_t err = esp_vfs_fat_spiflash_unmount_rw_wl(STORAGE_FAT_MOUNT_POINT, s_wl_handle);
    if (err == ESP_OK) {
        s_fat_mounted = false;
        s_wl_handle = WL_INVALID_HANDLE;
        ESP_LOGI(TAG, "FAT-раздел успешно размонтирован");
    } else {
        ESP_LOGE(TAG, "Ошибка размонтирования FAT: %s", esp_err_to_name(err));
    }

    return err;
}

// ============================================================================
// NVS: u8
// ============================================================================

esp_err_t storage_nvs_get_u8(const char *key, uint8_t *out_value, uint8_t default_value)
{
    if (out_value == NULL || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_value = default_value;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
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
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open (RW) ошибка при записи '%s': %s", key, esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(handle, key, value);
    if (err == ESP_OK) {
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
    if (out_buf == NULL || buf_size == 0 || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

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
        ESP_LOGW(TAG, "nvs_get_str('%s') ошибка: %s", key, esp_err_to_name(err));
    }

    return err;
}

esp_err_t storage_nvs_set_str(const char *key, const char *value)
{
    if (key == NULL || value == NULL) {
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
