#include "file_service.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "storage_sd.h"

#define LEGACY_STREAM_SLOTS 8
typedef struct { void *object; uint32_t generation; bool directory; } legacy_slot_t;
static SemaphoreHandle_t s_legacy_lock;
static legacy_slot_t s_legacy[LEGACY_STREAM_SLOTS];

static esp_err_t from_errno(void)
{
    switch (errno) {
        case ENOENT: return ESP_ERR_NOT_FOUND;
        case ENOMEM: return ESP_ERR_NO_MEM;
        case EINVAL: return ESP_ERR_INVALID_ARG;
        case ENOSPC: return ESP_ERR_NO_MEM;
        case EBUSY: return ESP_ERR_INVALID_STATE;
        case EEXIST: return ESP_ERR_INVALID_STATE;
        default: return ESP_FAIL;
    }
}

esp_err_t file_service_init(void)
{
    if (s_legacy_lock == NULL) {
        s_legacy_lock = xSemaphoreCreateMutex();
        if (s_legacy_lock == NULL) return ESP_ERR_NO_MEM;
    }
    esp_err_t err = storage_manager_init();
    if (err != ESP_OK) return err;
    return storage_manager_create_layout();
}

static bool lease_path(const char *path, storage_volume_id_t *volume,
                       uint32_t *generation)
{
    if (storage_manager_validate_path(path, volume) != ESP_OK) return false;
    return *volume != STORAGE_VOLUME_SD ||
           storage_sd_acquire(generation) == ESP_OK;
}

static void release_path(storage_volume_id_t volume, uint32_t generation)
{
    if (volume == STORAGE_VOLUME_SD) storage_sd_release(generation);
}

esp_err_t file_service_open(const char *path, const char *mode,
                            file_service_file_t *file)
{
    if (mode == NULL || file == NULL) return ESP_ERR_INVALID_ARG;
    memset(file, 0, sizeof(*file));
    esp_err_t err = storage_manager_validate_path(path, &file->volume);
    if (err != ESP_OK) return err;
    if (file->volume == STORAGE_VOLUME_SD) {
        err = storage_sd_acquire(&file->generation);
        if (err != ESP_OK) return err;
        file->leased = true;
    }
    file->stream = fopen(path, mode);
    if (file->stream == NULL) {
        err = from_errno();
        if (file->leased) storage_sd_release(file->generation);
        memset(file, 0, sizeof(*file));
        return err;
    }
    return ESP_OK;
}

esp_err_t file_service_close(file_service_file_t *file)
{
    if (file == NULL || file->stream == NULL) return ESP_ERR_INVALID_ARG;
    int rc = fclose(file->stream);
    if (file->leased) storage_sd_release(file->generation);
    memset(file, 0, sizeof(*file));
    return rc == 0 ? ESP_OK : from_errno();
}

esp_err_t file_service_flush_sync(file_service_file_t *file)
{
    if (file == NULL || file->stream == NULL) return ESP_ERR_INVALID_ARG;
    if (fflush(file->stream) != 0) return from_errno();
    return fsync(fileno(file->stream)) == 0 ? ESP_OK : from_errno();
}

esp_err_t file_service_stat(const char *path, struct stat *st)
{
    storage_volume_id_t volume; uint32_t generation = 0;
    if (!lease_path(path, &volume, &generation)) return ESP_ERR_NOT_FOUND;
    esp_err_t err = stat(path, st) == 0 ? ESP_OK : from_errno();
    release_path(volume, generation);
    return err;
}

bool file_service_exists(const char *path)
{
    struct stat st;
    return file_service_stat(path, &st) == ESP_OK;
}

esp_err_t file_service_mkdir(const char *path)
{
    storage_volume_id_t volume; uint32_t generation = 0;
    if (!lease_path(path, &volume, &generation)) return ESP_ERR_NOT_FOUND;
    esp_err_t err;
    if (mkdir(path, 0775) == 0) err = ESP_OK;
    else if (errno == EEXIST) { struct stat st; err = stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? ESP_OK : ESP_ERR_INVALID_STATE; }
    else err = from_errno();
    release_path(volume, generation);
    return err;
}

esp_err_t file_service_remove(const char *path)
{
    storage_volume_id_t volume; uint32_t generation = 0;
    if (!lease_path(path, &volume, &generation)) return ESP_ERR_NOT_FOUND;
    esp_err_t err = remove(path) == 0 ? ESP_OK : from_errno();
    release_path(volume, generation);
    return err;
}

esp_err_t file_service_rename(const char *from, const char *to)
{
    storage_volume_id_t a, b;
    esp_err_t err = storage_manager_validate_path(from, &a);
    if (err != ESP_OK) return err;
    err = storage_manager_validate_path(to, &b);
    if (err != ESP_OK) return err;
    if (a != b) return ESP_ERR_NOT_SUPPORTED;
    uint32_t generation = 0;
    if (a == STORAGE_VOLUME_SD && storage_sd_acquire(&generation) != ESP_OK)
        return ESP_ERR_NOT_FOUND;
    err = rename(from, to) == 0 ? ESP_OK : from_errno();
    release_path(a, generation);
    return err;
}

static bool legacy_register(void *object, uint32_t generation, bool directory)
{
    if (s_legacy_lock == NULL || xSemaphoreTake(s_legacy_lock, portMAX_DELAY) != pdTRUE) return false;
    bool stored = false;
    for (size_t i = 0; i < LEGACY_STREAM_SLOTS; ++i) if (s_legacy[i].object == NULL) {
        s_legacy[i] = (legacy_slot_t){ object, generation, directory }; stored = true; break;
    }
    xSemaphoreGive(s_legacy_lock); return stored;
}

static uint32_t legacy_take(void *object, bool directory, bool *found)
{
    uint32_t generation = 0; *found = false;
    if (s_legacy_lock == NULL || xSemaphoreTake(s_legacy_lock, portMAX_DELAY) != pdTRUE) return 0;
    for (size_t i = 0; i < LEGACY_STREAM_SLOTS; ++i)
        if (s_legacy[i].object == object && s_legacy[i].directory == directory) {
            generation = s_legacy[i].generation; memset(&s_legacy[i], 0, sizeof(s_legacy[i])); *found = true; break;
        }
    xSemaphoreGive(s_legacy_lock); return generation;
}

FILE *file_service_fopen_stream(const char *path, const char *mode)
{
    storage_volume_id_t volume; uint32_t generation = 0;
    if (!lease_path(path, &volume, &generation)) { errno = ENODEV; return NULL; }
    FILE *stream = fopen(path, mode);
    if (stream == NULL) { release_path(volume, generation); return NULL; }
    if (volume == STORAGE_VOLUME_SD && !legacy_register(stream, generation, false)) {
        fclose(stream); storage_sd_release(generation); errno = EMFILE; return NULL;
    }
    return stream;
}

int file_service_fclose_stream(FILE *stream)
{
    bool found; uint32_t generation = legacy_take(stream, false, &found);
    int rc = fclose(stream); if (found) storage_sd_release(generation); return rc;
}

DIR *file_service_opendir_stream(const char *path)
{
    storage_volume_id_t volume; uint32_t generation = 0;
    if (!lease_path(path, &volume, &generation)) { errno = ENODEV; return NULL; }
    DIR *directory = opendir(path);
    if (directory == NULL) { release_path(volume, generation); return NULL; }
    if (volume == STORAGE_VOLUME_SD && !legacy_register(directory, generation, true)) {
        closedir(directory); storage_sd_release(generation); errno = EMFILE; return NULL;
    }
    return directory;
}

int file_service_closedir_stream(DIR *directory)
{
    bool found; uint32_t generation = legacy_take(directory, true, &found);
    int rc = closedir(directory); if (found) storage_sd_release(generation); return rc;
}

int file_service_stat_posix(const char *path, struct stat *st) { return file_service_stat(path, st) == ESP_OK ? 0 : -1; }
int file_service_mkdir_posix(const char *path, mode_t mode) { (void)mode; return file_service_mkdir(path) == ESP_OK ? 0 : -1; }
int file_service_remove_posix(const char *path) { return file_service_remove(path) == ESP_OK ? 0 : -1; }
int file_service_rename_posix(const char *from, const char *to) { return file_service_rename(from, to) == ESP_OK ? 0 : -1; }

esp_err_t file_service_space(storage_volume_id_t volume, uint64_t *free_bytes,
                             uint64_t *total_bytes)
{
    storage_volume_snapshot_t v;
    if (free_bytes == NULL || total_bytes == NULL) return ESP_ERR_INVALID_ARG;
    if (!storage_manager_get_volume(volume, &v)) return ESP_ERR_INVALID_STATE;
    if (!v.mounted) return ESP_ERR_NOT_FOUND;
    *free_bytes = v.free_bytes; *total_bytes = v.total_bytes;
    return ESP_OK;
}

esp_err_t file_service_resolve(storage_role_t role, const char *leaf,
                               char *out, size_t out_size)
{
    return storage_manager_resolve(role, leaf, out, out_size);
}

esp_err_t file_service_make_unique(storage_role_t role, const char *stem,
                                   const char *extension, char *out,
                                   size_t out_size)
{
    if (stem == NULL || stem[0] == '\0' || extension == NULL ||
        extension[0] == '\0' || out == NULL) return ESP_ERR_INVALID_ARG;
    char directory[STORAGE_MANAGER_PATH_MAX];
    esp_err_t err = file_service_resolve(role, NULL, directory,
                                         sizeof(directory));
    if (err != ESP_OK) return err;

    DIR *dir = file_service_opendir_stream(directory);
    if (dir == NULL) return from_errno();

    const size_t stem_len = strlen(stem);
    const size_t extension_len = strlen(extension);
    uint64_t maximum = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        size_t name_len = strlen(name);
        if (name_len <= stem_len + 1U + extension_len ||
            memcmp(name, stem, stem_len) != 0 || name[stem_len] != '_' ||
            memcmp(name + name_len - extension_len, extension,
                   extension_len) != 0) continue;

        const char *digits = name + stem_len + 1U;
        const char *digits_end = name + name_len - extension_len;
        bool numeric = digits < digits_end;
        for (const char *p = digits; numeric && p < digits_end; ++p)
            numeric = *p >= '0' && *p <= '9';
        if (!numeric) continue;

        errno = 0;
        char *parsed_end = NULL;
        unsigned long long value = strtoull(digits, &parsed_end, 10);
        if (errno == ERANGE || parsed_end != digits_end || value == 0U)
            continue;
        if ((uint64_t)value > maximum) maximum = (uint64_t)value;
    }
    if (file_service_closedir_stream(dir) != 0) return from_errno();
    if (maximum == UINT64_MAX) return ESP_ERR_NO_MEM;

    char leaf[96];
    int n = snprintf(leaf, sizeof(leaf), "%s_%llu%s", stem,
                     (unsigned long long)(maximum + 1U), extension);
    if (n < 0 || (size_t)n >= sizeof(leaf)) return ESP_ERR_INVALID_SIZE;
    return file_service_resolve(role, leaf, out, out_size);
}
