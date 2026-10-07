#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_MANAGER_PATH_MAX 256
#define STORAGE_VOLUME_CAP_READ   (1U << 0)
#define STORAGE_VOLUME_CAP_WRITE  (1U << 1)
#define STORAGE_VOLUME_CAP_REMOVE (1U << 2)

typedef enum {
    STORAGE_VOLUME_INTERNAL = 0,
    STORAGE_VOLUME_SD,
    STORAGE_VOLUME_COUNT
} storage_volume_id_t;

typedef enum {
    STORAGE_TYPE_INTERNAL_FLASH = 0,
    STORAGE_TYPE_SD_CARD
} storage_volume_type_t;

typedef enum {
    STORAGE_ROLE_SYSTEM = 0,
    STORAGE_ROLE_CONFIG,
    STORAGE_ROLE_STATE,
    STORAGE_ROLE_INDEX,
    STORAGE_ROLE_CACHE,
    STORAGE_ROLE_TEMP,
    STORAGE_ROLE_PROJECTS,
    STORAGE_ROLE_CAPTURE_WIFI_PCAP,
    STORAGE_ROLE_CAPTURE_WIFI_HANDSHAKES,
    STORAGE_ROLE_CAPTURE_WIFI_EVENTS,
    STORAGE_ROLE_CAPTURE_SUBGHZ,
    STORAGE_ROLE_SESSIONS,
    STORAGE_ROLE_EXPORTS,
    STORAGE_ROLE_DOWNLOADS
} storage_role_t;

typedef struct {
    storage_volume_id_t id;
    storage_volume_type_t type;
    char label[16];
    char mount_point[16];
    bool mounted;
    uint64_t total_bytes;
    uint64_t free_bytes;
    uint32_t capabilities;
    uint32_t generation;
    esp_err_t last_error;
} storage_volume_snapshot_t;

esp_err_t storage_manager_init(void);
esp_err_t storage_manager_refresh(void);
bool storage_manager_get_volume(storage_volume_id_t id,
                                storage_volume_snapshot_t *out);
esp_err_t storage_manager_resolve(storage_role_t role, const char *leaf,
                                  char *out, size_t out_size);
esp_err_t storage_manager_validate_path(const char *path,
                                        storage_volume_id_t *volume);
esp_err_t storage_manager_create_layout(void);

#ifdef __cplusplus
}
#endif
