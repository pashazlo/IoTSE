#include "storage_manager.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "storage.h"
#include "storage_sd.h"

typedef struct { storage_volume_id_t volume; const char *path; } role_map_t;

static SemaphoreHandle_t s_lock;
static storage_volume_snapshot_t s_volumes[STORAGE_VOLUME_COUNT];

static const role_map_t s_roles[] = {
    [STORAGE_ROLE_SYSTEM] = { STORAGE_VOLUME_INTERNAL, "/storage" },
    [STORAGE_ROLE_CONFIG] = { STORAGE_VOLUME_INTERNAL, "/storage/config" },
    [STORAGE_ROLE_STATE] = { STORAGE_VOLUME_INTERNAL, "/storage/state" },
    [STORAGE_ROLE_INDEX] = { STORAGE_VOLUME_INTERNAL, "/storage/index" },
    [STORAGE_ROLE_CACHE] = { STORAGE_VOLUME_INTERNAL, "/storage/cache" },
    [STORAGE_ROLE_TEMP] = { STORAGE_VOLUME_INTERNAL, "/storage/tmp" },
    [STORAGE_ROLE_PROJECTS] = { STORAGE_VOLUME_SD, "/sdcard/Projects" },
    [STORAGE_ROLE_CAPTURE_WIFI_PCAP] = { STORAGE_VOLUME_SD, "/sdcard/Captures/WiFi/PCAP" },
    [STORAGE_ROLE_CAPTURE_WIFI_HANDSHAKES] = { STORAGE_VOLUME_SD, "/sdcard/Captures/WiFi/Handshakes" },
    [STORAGE_ROLE_CAPTURE_WIFI_EVENTS] = { STORAGE_VOLUME_SD, "/sdcard/Captures/WiFi/Events" },
    [STORAGE_ROLE_CAPTURE_SUBGHZ] = { STORAGE_VOLUME_SD, "/sdcard/Captures/SubGHz" },
    [STORAGE_ROLE_SESSIONS] = { STORAGE_VOLUME_SD, "/sdcard/Sessions" },
    [STORAGE_ROLE_EXPORTS] = { STORAGE_VOLUME_SD, "/sdcard/Exports" },
    [STORAGE_ROLE_DOWNLOADS] = { STORAGE_VOLUME_SD, "/sdcard/Downloads" },
};

static void update_volume(storage_volume_id_t id, bool mounted,
                          uint64_t known_capacity, esp_err_t last_error)
{
    storage_volume_snapshot_t *v = &s_volumes[id];
    uint64_t total = known_capacity, free_bytes = 0;
    if (mounted) {
        struct statvfs fs;
        if (statvfs(v->mount_point, &fs) == 0) {
            total = (uint64_t)fs.f_blocks * fs.f_frsize;
            free_bytes = (uint64_t)fs.f_bavail * fs.f_frsize;
        }
    }
    if (v->mounted != mounted || v->total_bytes != total) {
        ++v->generation;
        if (v->generation == 0) ++v->generation;
    }
    v->mounted = mounted;
    v->total_bytes = mounted ? total : 0;
    v->free_bytes = mounted ? free_bytes : 0;
    v->last_error = last_error;
}

esp_err_t storage_manager_init(void)
{
    if (s_lock != NULL) return storage_manager_refresh();
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    s_volumes[STORAGE_VOLUME_INTERNAL] = (storage_volume_snapshot_t) {
        .id = STORAGE_VOLUME_INTERNAL, .type = STORAGE_TYPE_INTERNAL_FLASH,
        .label = "Internal", .mount_point = STORAGE_FAT_MOUNT_POINT,
        .capabilities = STORAGE_VOLUME_CAP_READ | STORAGE_VOLUME_CAP_WRITE |
                        STORAGE_VOLUME_CAP_REMOVE,
    };
    s_volumes[STORAGE_VOLUME_SD] = (storage_volume_snapshot_t) {
        .id = STORAGE_VOLUME_SD, .type = STORAGE_TYPE_SD_CARD,
        .label = "SD Card", .mount_point = STORAGE_SD_MOUNT_POINT,
        .capabilities = STORAGE_VOLUME_CAP_READ | STORAGE_VOLUME_CAP_WRITE |
                        STORAGE_VOLUME_CAP_REMOVE,
    };
    return storage_manager_refresh();
}

esp_err_t storage_manager_refresh(void)
{
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    storage_sd_snapshot_t sd = { 0 };
    bool have_sd = storage_sd_get_snapshot(&sd);
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    update_volume(STORAGE_VOLUME_INTERNAL, storage_fat_is_mounted(), 0, ESP_OK);
    update_volume(STORAGE_VOLUME_SD,
                  have_sd && sd.state == STORAGE_SD_MOUNTED,
                  have_sd ? sd.capacity_bytes : 0,
                  have_sd ? sd.last_error : ESP_ERR_INVALID_STATE);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool storage_manager_get_volume(storage_volume_id_t id,
                                storage_volume_snapshot_t *out)
{
    if (out == NULL || id >= STORAGE_VOLUME_COUNT || s_lock == NULL) return false;
    (void)storage_manager_refresh();
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) != pdTRUE) return false;
    *out = s_volumes[id];
    xSemaphoreGive(s_lock);
    return true;
}

static bool bad_leaf(const char *leaf)
{
    if (leaf == NULL || leaf[0] == '\0') return false;
    if (leaf[0] == '/' || leaf[0] == '\\') return true;
    const char *p = leaf;
    while (*p) {
        const char *start = p;
        while (*p && *p != '/' && *p != '\\') ++p;
        if ((p - start) == 2 && start[0] == '.' && start[1] == '.') return true;
        if (*p) ++p;
    }
    return false;
}

esp_err_t storage_manager_resolve(storage_role_t role, const char *leaf,
                                  char *out, size_t out_size)
{
    if (role >= sizeof(s_roles) / sizeof(s_roles[0]) || out == NULL ||
        out_size == 0 || bad_leaf(leaf)) return ESP_ERR_INVALID_ARG;
    storage_volume_snapshot_t volume;
    if (!storage_manager_get_volume(s_roles[role].volume, &volume))
        return ESP_ERR_INVALID_STATE;
    if (!volume.mounted) return ESP_ERR_NOT_FOUND;
    int n = (leaf != NULL && leaf[0] != '\0')
        ? snprintf(out, out_size, "%s/%s", s_roles[role].path, leaf)
        : snprintf(out, out_size, "%s", s_roles[role].path);
    return n >= 0 && (size_t)n < out_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t storage_manager_validate_path(const char *path,
                                        storage_volume_id_t *volume)
{
    if (path == NULL || path[0] != '/' ||
        strnlen(path, STORAGE_MANAGER_PATH_MAX) >= STORAGE_MANAGER_PATH_MAX ||
        bad_leaf(path + 1))
        return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < STORAGE_VOLUME_COUNT; ++i) {
        storage_volume_snapshot_t v;
        if (!storage_manager_get_volume((storage_volume_id_t)i, &v)) continue;
        size_t len = strlen(v.mount_point);
        if (strncmp(path, v.mount_point, len) == 0 &&
            (path[len] == '\0' || path[len] == '/')) {
            if (!v.mounted) return ESP_ERR_NOT_FOUND;
            if (volume != NULL) *volume = v.id;
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode) ? ESP_OK : ESP_ERR_INVALID_STATE;
    return mkdir(path, 0775) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t storage_manager_create_layout(void)
{
    static const char *internal[] = {
        "/storage/config", "/storage/state", "/storage/index",
        "/storage/cache", "/storage/tmp"
    };
    static const char *sd[] = {
        "/sdcard/Projects", "/sdcard/Captures", "/sdcard/Captures/WiFi",
        "/sdcard/Captures/WiFi/PCAP", "/sdcard/Captures/WiFi/Handshakes",
        "/sdcard/Captures/WiFi/Events", "/sdcard/Captures/SubGHz",
        "/sdcard/Sessions", "/sdcard/Exports", "/sdcard/Downloads"
    };
    esp_err_t first = ESP_OK;
    storage_volume_snapshot_t v;
    if (storage_manager_get_volume(STORAGE_VOLUME_INTERNAL, &v) && v.mounted)
        for (size_t i = 0; i < sizeof(internal)/sizeof(internal[0]); ++i) {
            esp_err_t err = ensure_dir(internal[i]); if (first == ESP_OK && err != ESP_OK) first = err;
        }
    if (storage_manager_get_volume(STORAGE_VOLUME_SD, &v) && v.mounted)
        for (size_t i = 0; i < sizeof(sd)/sizeof(sd[0]); ++i) {
            esp_err_t err = ensure_dir(sd[i]); if (first == ESP_OK && err != ESP_OK) first = err;
        }
    return first;
}
