#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_err.h"
#include "storage_manager.h"

typedef struct {
    FILE *stream;
    storage_volume_id_t volume;
    uint32_t generation;
    bool leased;
} file_service_file_t;

esp_err_t file_service_init(void);
esp_err_t file_service_open(const char *path, const char *mode,
                            file_service_file_t *file);
esp_err_t file_service_close(file_service_file_t *file);
esp_err_t file_service_flush_sync(file_service_file_t *file);
esp_err_t file_service_stat(const char *path, struct stat *st);
bool file_service_exists(const char *path);
esp_err_t file_service_mkdir(const char *path);
esp_err_t file_service_remove(const char *path);
esp_err_t file_service_rename(const char *from, const char *to);
esp_err_t file_service_space(storage_volume_id_t volume, uint64_t *free_bytes,
                             uint64_t *total_bytes);
esp_err_t file_service_resolve(storage_role_t role, const char *leaf,
                               char *out, size_t out_size);
esp_err_t file_service_make_unique(storage_role_t role, const char *stem,
                                   const char *extension, char *out,
                                   size_t out_size);

/* POSIX-shaped adapters for legacy FM/editor code. They still validate paths
 * and hold an SD lifecycle lease until fclose/closedir. New code should prefer
 * the explicit esp_err_t API above. */
FILE *file_service_fopen_stream(const char *path, const char *mode);
int file_service_fclose_stream(FILE *stream);
DIR *file_service_opendir_stream(const char *path);
int file_service_closedir_stream(DIR *directory);
int file_service_stat_posix(const char *path, struct stat *st);
int file_service_mkdir_posix(const char *path, mode_t mode);
int file_service_remove_posix(const char *path);
int file_service_rename_posix(const char *from, const char *to);
