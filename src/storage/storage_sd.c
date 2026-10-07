#include "storage_sd.h"

#include <string.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"
#include "spi_bus.h"

static const char *TAG = "storage_sd";

static SemaphoreHandle_t s_lock;
static storage_sd_snapshot_t s_snapshot;
static sdmmc_card_t *s_card;
static bool s_uses_shared_spi;

#define SD_SPI_LOCK_WAIT_MS 2000
#define SD_SPI_MAX_FREQ_KHZ 4000
#define SD_SPI_FALLBACK_FREQ_KHZ 1000

static esp_vfs_fat_mount_config_t mount_config(void)
{
    return (esp_vfs_fat_mount_config_t) {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = 0,
    };
}

static void finish_mount(esp_err_t err, sdmmc_card_t *card, bool shared_spi)
{
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return;
    s_snapshot.last_error = err;
    if (err == ESP_OK && card != NULL) {
        s_card = card;
        s_uses_shared_spi = shared_spi;
        s_snapshot.capacity_bytes =
            (uint64_t)card->csd.capacity * card->csd.sector_size;
        ++s_snapshot.generation;
        if (s_snapshot.generation == 0U) ++s_snapshot.generation;
        s_snapshot.state = STORAGE_SD_MOUNTED;
    } else {
        s_card = NULL;
        s_uses_shared_spi = false;
        s_snapshot.capacity_bytes = 0U;
        s_snapshot.state = STORAGE_SD_ERROR;
    }
    xSemaphoreGive(s_lock);
}

static esp_err_t begin_mount(void)
{
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    esp_err_t err = ESP_OK;
    if (s_snapshot.state == STORAGE_SD_MOUNTED ||
        s_snapshot.state == STORAGE_SD_MOUNTING ||
        s_snapshot.state == STORAGE_SD_EJECTING) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        s_snapshot.state = STORAGE_SD_MOUNTING;
        s_snapshot.last_error = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t storage_sd_init(void)
{
    if (s_lock != NULL) return ESP_OK;
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    if (lock == NULL) return ESP_ERR_NO_MEM;
    s_lock = lock;
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.state = STORAGE_SD_UNMOUNTED;
    return ESP_OK;
}

esp_err_t storage_sd_mount_sdmmc(
    const sdmmc_host_t *host,
    const sdmmc_slot_config_t *slot
)
{
    if (host == NULL || slot == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = begin_mount();
    if (err != ESP_OK) return err;

    sdmmc_card_t *card = NULL;
    esp_vfs_fat_mount_config_t config = mount_config();
    err = esp_vfs_fat_sdmmc_mount(
        STORAGE_SD_MOUNT_POINT, host, slot, &config, &card);
    finish_mount(err, card, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC mount failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t storage_sd_mount_sdspi(
    const sdmmc_host_t *host,
    const sdspi_device_config_t *device
)
{
    if (host == NULL || device == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = begin_mount();
    if (err != ESP_OK) return err;

    const bool shared_spi = device->host_id == SHARED_SPI_HOST;
    if (shared_spi &&
        !spi_bus_lock(pdMS_TO_TICKS(SD_SPI_LOCK_WAIT_MS))) {
        finish_mount(ESP_ERR_TIMEOUT, NULL, false);
        return ESP_ERR_TIMEOUT;
    }

    sdmmc_card_t *card = NULL;
    esp_vfs_fat_mount_config_t config = mount_config();
    err = esp_vfs_fat_sdspi_mount(
        STORAGE_SD_MOUNT_POINT, host, device, &config, &card);
    if (shared_spi) spi_bus_unlock();
    finish_mount(err, card, shared_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SDSPI mount failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t storage_sd_mount_board(void)
{
    esp_err_t err = spi_bus_shared_init();
    if (err != ESP_OK) return err;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SHARED_SPI_HOST;
    /* Long jumper wires and inexpensive modules are often unreliable at the
     * 20 MHz default. Start conservatively; throughput can be raised after
     * repeated read/write verification on the final PCB. */
    host.max_freq_khz = SD_SPI_MAX_FREQ_KHZ;
    sdspi_device_config_t device = SDSPI_DEVICE_CONFIG_DEFAULT();
    device.host_id = SHARED_SPI_HOST;
    device.gpio_cs = SPI_BUS_SD_CS_GPIO;
    ESP_LOGI(TAG, "SDSPI mount attempt at %u kHz", SD_SPI_MAX_FREQ_KHZ);
    err = storage_sd_mount_sdspi(&host, &device);
    if (err == ESP_ERR_INVALID_CRC) {
        ESP_LOGW(TAG, "CRC failure; retrying SDSPI at %u kHz",
                 SD_SPI_FALLBACK_FREQ_KHZ);
        host.max_freq_khz = SD_SPI_FALLBACK_FREQ_KHZ;
        err = storage_sd_mount_sdspi(&host, &device);
    }
    return err;
}

esp_err_t storage_sd_unmount(void)
{
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    if (s_snapshot.state != STORAGE_SD_MOUNTED || s_card == NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_snapshot.active_io != 0U) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_snapshot.state = STORAGE_SD_EJECTING;
    sdmmc_card_t *card = s_card;
    bool shared_spi = s_uses_shared_spi;
    xSemaphoreGive(s_lock);

    if (shared_spi &&
        !spi_bus_lock(pdMS_TO_TICKS(SD_SPI_LOCK_WAIT_MS))) {
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            s_snapshot.state = STORAGE_SD_MOUNTED;
            s_snapshot.last_error = ESP_ERR_TIMEOUT;
            xSemaphoreGive(s_lock);
        }
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = esp_vfs_fat_sdcard_unmount(STORAGE_SD_MOUNT_POINT, card);
    if (shared_spi) spi_bus_unlock();
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    s_snapshot.last_error = err;
    if (err == ESP_OK) {
        s_card = NULL;
        s_uses_shared_spi = false;
        s_snapshot.capacity_bytes = 0U;
        ++s_snapshot.generation;
        if (s_snapshot.generation == 0U) ++s_snapshot.generation;
        s_snapshot.state = STORAGE_SD_UNMOUNTED;
    } else {
        /* The VFS/card are still owned by us when unmount fails. Keep the
         * volume mounted so callers can retry; entering ERROR here would
         * incorrectly permit a second mount over the live VFS. */
        s_snapshot.state = STORAGE_SD_MOUNTED;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t storage_sd_acquire(uint32_t *generation)
{
    if (generation == NULL) return ESP_ERR_INVALID_ARG;
    if (s_lock == NULL || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    if (s_snapshot.state != STORAGE_SD_MOUNTED) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        ++s_snapshot.active_io;
        *generation = s_snapshot.generation;
    }
    xSemaphoreGive(s_lock);
    return err;
}

void storage_sd_release(uint32_t generation)
{
    if (s_lock == NULL ||
        xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) return;
    if (generation == s_snapshot.generation && s_snapshot.active_io > 0U) {
        --s_snapshot.active_io;
    }
    xSemaphoreGive(s_lock);
}

bool storage_sd_is_mounted(void)
{
    storage_sd_snapshot_t snapshot;
    return storage_sd_get_snapshot(&snapshot) &&
           snapshot.state == STORAGE_SD_MOUNTED;
}

bool storage_sd_get_snapshot(storage_sd_snapshot_t *snapshot)
{
    if (snapshot == NULL || s_lock == NULL ||
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) return false;
    *snapshot = s_snapshot;
    xSemaphoreGive(s_lock);
    return true;
}
