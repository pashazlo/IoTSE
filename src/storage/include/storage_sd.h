#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_SD_MOUNT_POINT "/sdcard"

typedef enum {
    STORAGE_SD_UNINITIALIZED = 0,
    STORAGE_SD_UNMOUNTED,
    STORAGE_SD_MOUNTING,
    STORAGE_SD_MOUNTED,
    STORAGE_SD_EJECTING,
    STORAGE_SD_ERROR,
} storage_sd_state_t;

typedef struct {
    storage_sd_state_t state;
    uint32_t generation;
    uint32_t active_io;
    uint64_t capacity_bytes;
    esp_err_t last_error;
} storage_sd_snapshot_t;

/*
 * This module owns only the card/VFS lifecycle. Board code owns the chosen
 * transport, pins and (for SDSPI) bus initialization. No GPIO is guessed here.
 * Mount never formats a card. All callers must serialize user-facing mount and
 * eject requests through a worker task.
 */
esp_err_t storage_sd_init(void);
esp_err_t storage_sd_mount_sdmmc(
    const sdmmc_host_t *host,
    const sdmmc_slot_config_t *slot
);
esp_err_t storage_sd_mount_sdspi(
    const sdmmc_host_t *host,
    const sdspi_device_config_t *device
);

/* Mount the board SDSPI socket on the already initialized shared SPI2 bus.
 * Wiring is defined centrally by spi_bus.h: SCK=18, MOSI=17, MISO=8, CS=3. */
esp_err_t storage_sd_mount_board(void);
esp_err_t storage_sd_unmount(void);

/* An IO lease prevents unmount while a filesystem operation is active. It
 * deliberately does not hold the shared SPI mutex for the file lifetime. */
esp_err_t storage_sd_acquire(uint32_t *generation);
void storage_sd_release(uint32_t generation);

bool storage_sd_is_mounted(void);
bool storage_sd_get_snapshot(storage_sd_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
