#include "fm.h"

#include <string.h>
#include <strings.h>     // strcasecmp()
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#include "esp_log.h"

static const char *TAG = "fm";

// ============================================================================
// Внутреннее состояние файлового менеджера
// ============================================================================

// Максимальное количество зарегистрированных томов.
//
// Сейчас тома могут быть:
//
//     Internal Storage
//     SD Card
//
// Позже можно добавить другие источники.
#define FM_MAX_VOLUMES   4
#define FM_DELETE_MAX_DEPTH 8

// Зарегистрированные тома.
//
// Файловый менеджер хранит только указатели на описания томов.
// Сами структуры fm_volume_t обычно создаются в main.c статически.
static const fm_volume_t *s_volumes[FM_MAX_VOLUMES];

// Количество реально зарегистрированных томов.
static uint8_t s_volume_count = 0;

// Индекс текущего выбранного тома.
//
// Значение имеет смысл только если s_volume_selected == true.
static uint8_t s_current_volume = 0;

// Был ли вообще выбран том.
//
// Это отдельное состояние необходимо, потому что индекс 0 сам по себе
// ещё не означает, что существует volume[0].
static bool s_volume_selected = false;

// Текущий путь внутри выбранного тома.
//
// Например:
//
//     /storage
//     /storage/scripts
//     /storage/scripts/test
//
static char s_current_path[FM_MAX_PATH_LEN];

// ============================================================================
// Кэш текущей директории
// ============================================================================

// Готовый список файлов и папок текущей директории.
//
// UI читает именно этот массив, а не вызывает opendir()/readdir()
// во время каждого кадра.
typedef struct {
fm_entry_t entries[FM_BROWSER_MAX_ENTRIES];
char path[FM_MAX_PATH_LEN];
uint8_t count;
} fm_cache_bank_t;

static fm_cache_bank_t s_cache_banks[2];
static uint8_t s_active_cache_bank = 0;

// ============================================================================
// Внутренние вспомогательные функции
// ============================================================================

/**

* @brief Проверить, что строка является обычным именем файла/папки,
* ```
     а не путём.
  ```
*
* Разрешено:
*
* ```
  file.txt
  ```
* ```
  scripts
  ```
* ```
  test.rts
  ```
*
* Запрещено:
*
* ```
  .
  ```
* ```
  ..
  ```
* ```
  ../file
  ```
* ```
  folder/file
  ```
* ```
  /absolute/path
  ```
*
* Это предотвращает случайный выход за пределы текущей директории
* через конструкции вроде "../".
  */
  static bool is_valid_name(const char *name)
  {
  if (name == NULL || name[0] == '\0') {
  return false;
  }

  // "." и ".." являются специальными элементами файловой системы,
  // поэтому не могут использоваться как обычные имена.
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
  return false;
  }

  // Обычное имя не должно содержать разделитель директорий.
  //
  // Это запрещает:
  //
  //     folder/file
  //     ../file
  //     /storage/file
  //
  if (strchr(name, '/') != NULL) {
  return false;
  }

  return true;
  }

/**

* @brief Проверить, выбран ли текущий том и существует ли его описание.
  */
  static bool has_current_volume(void)
  {
  if (!s_volume_selected) {
  return false;
  }

  if (s_current_volume >= s_volume_count) {
  return false;
  }

  if (s_volumes[s_current_volume] == NULL) {
  return false;
  }

  return true;
  }

/**

* @brief Проверить существование записи.
*
* Используется перед созданием файлов/папок и переименованием,
* чтобы случайно не перезаписать уже существующие данные.
  */
static bool path_exists(const char *path)
  {
  struct stat st;

  return stat(path, &st) == 0;
  }

static bool strip_internal_suffix(
    const char *name,
    const char *suffix,
    char *out_name,
    size_t out_size
)
{
size_t name_len = strlen(name);
size_t suffix_len = strlen(suffix);
if (name_len <= suffix_len ||
    strcmp(name + name_len - suffix_len, suffix) != 0) {
    return false;
}
size_t base_len = name_len - suffix_len;
if (base_len >= out_size) {
    return false;
}
memcpy(out_name, name, base_len);
out_name[base_len] = '\0';
return true;
}

static bool has_internal_transaction_suffix(const char *name)
{
char unused[FM_MAX_NAME_LEN];
return strip_internal_suffix(name, ".iotse.bak", unused, sizeof(unused)) ||
       strip_internal_suffix(name, ".iotse.tmp", unused, sizeof(unused));
}

// ============================================================================
// Регистрация томов
// ============================================================================

void fm_register_volume(const fm_volume_t *volume)
{
// Защита от NULL.
if (volume == NULL) {
ESP_LOGE(TAG, "Попытка зарегистрировать NULL-том");
return;
}

// Минимальная проверка структуры тома.
if (volume->label == NULL || volume->mount_point == NULL) {
    ESP_LOGE(TAG, "Том имеет NULL label или mount_point");
    return;
}

if (s_volume_count >= FM_MAX_VOLUMES) {
    ESP_LOGW(
        TAG,
        "Достигнут лимит томов (%d), '%s' не зарегистрирован",
        FM_MAX_VOLUMES,
        volume->label
    );

    return;
}

s_volumes[s_volume_count++] = volume;

ESP_LOGI(
    TAG,
    "Том '%s' зарегистрирован (%s)",
    volume->label,
    volume->mount_point
);

}

uint8_t fm_volume_count(void)
{
return s_volume_count;
}

const fm_volume_t *fm_get_volume(uint8_t index)
{
if (index >= s_volume_count) {
return NULL;
}

return s_volumes[index];

}

// ============================================================================
// Сортировка записей
// ============================================================================

// Сначала папки, затем файлы.
//
// Внутри каждой группы сортировка идёт по алфавиту без учёта регистра.
//
// Например:
//
//     Scripts
//     System
//     test
//     alpha.txt
//     beta.txt
//
static int compare_entries(const void *a, const void *b)
{
const fm_entry_t *ea = (const fm_entry_t *)a;
const fm_entry_t *eb = (const fm_entry_t *)b;

// Папки всегда выше файлов.
if (ea->is_dir != eb->is_dir) {
    return ea->is_dir ? -1 : 1;
}

// Алфавитная сортировка без учёта регистра.
return strcasecmp(ea->name, eb->name);

}

// ============================================================================
// Внутренний листинг директории
// ============================================================================

/**

* @brief Прочитать текущую директорию напрямую из файловой системы.
*
* Эта функция не является частью публичного API.
*
* UI никогда не должен вызывать её напрямую.
  */
  static esp_err_t list_current_dir_uncached(
  fm_entry_t *out_entries,
  uint8_t max_entries,
  uint8_t *out_count
  )
  {
  if (out_entries == NULL || out_count == NULL) {
  return ESP_ERR_INVALID_ARG;
  }

  *out_count = 0;

  // Нельзя читать директорию, пока пользователь не вошёл ни в один том.
  if (!has_current_volume()) {
  return ESP_ERR_INVALID_STATE;
  }

  DIR *dir = opendir(s_current_path);

  if (dir == NULL) {
  ESP_LOGE(
  TAG,
  "Не удалось открыть директорию '%s'",
  s_current_path
  );

   return ESP_FAIL;


  }

  uint8_t count = 0;
  struct dirent *ent;

  while (
  count < max_entries &&
  (ent = readdir(dir)) != NULL
  ) {

   // "." и ".." — служебные элементы POSIX.
   // В пользовательском файловом менеджере они не отображаются.
   if (
       strcmp(ent->d_name, ".") == 0 ||
       strcmp(ent->d_name, "..") == 0
   ) {
       continue;
   }


   fm_entry_t *entry = &out_entries[count];
   const char *display_name = ent->d_name;
   char recovered_name[FM_MAX_NAME_LEN];
   bool is_backup = strip_internal_suffix(
       ent->d_name, ".iotse.bak", recovered_name, sizeof(recovered_name));
   bool is_temp = !is_backup && strip_internal_suffix(
       ent->d_name, ".iotse.tmp", recovered_name, sizeof(recovered_name));

   if (is_backup || is_temp) {
       char canonical_path[FM_MAX_PATH_LEN];
       if (fm_build_full_path(recovered_name, canonical_path,
                              sizeof(canonical_path)) != ESP_OK) {
           continue;
       }
       bool canonical_exists = path_exists(canonical_path);
       if (!canonical_exists && is_temp) {
           char backup_name[FM_MAX_NAME_LEN + sizeof(".iotse.bak")];
           static const char backup_suffix[] = ".iotse.bak";
           size_t recovered_len = strlen(recovered_name);
           char backup_path[FM_MAX_PATH_LEN];
           if (recovered_len + sizeof(backup_suffix) > sizeof(backup_name)) {
               continue;
           }
           memcpy(backup_name, recovered_name, recovered_len);
           memcpy(backup_name + recovered_len, backup_suffix,
                  sizeof(backup_suffix));
           if (fm_build_full_path(backup_name, backup_path,
                                  sizeof(backup_path)) != ESP_OK) {
               continue;
           }
           if (path_exists(backup_path)) {
               continue;
           }
       }
       if (!canonical_exists) {
           display_name = recovered_name;
       }
   }


   // Копируем имя безопасно.
   strncpy(
       entry->name,
       display_name,
       FM_MAX_NAME_LEN - 1
   );

   entry->name[FM_MAX_NAME_LEN - 1] = '\0';


   // Собираем полный путь для stat().
   char full_path[FM_MAX_PATH_LEN];

   esp_err_t err = fm_build_full_path(
       ent->d_name,
       full_path,
       sizeof(full_path)
   );

   if (err != ESP_OK) {
       ESP_LOGW(
           TAG,
           "Путь для '%s' слишком длинный или некорректный",
           ent->d_name
       );

       // Не добавляем битую запись в список.
       continue;
   }


   // Получаем информацию о записи.
   struct stat st;

   if (stat(full_path, &st) == 0) {

       entry->is_dir = S_ISDIR(st.st_mode);

       // Для папок размер в UI сейчас не используется.
       entry->size = entry->is_dir
           ? 0
           : (uint32_t)st.st_size;

   } else {

       // Если stat() не удался, запись всё равно существует
       // (её вернул readdir), но мы не смогли определить тип.
       //
       // Считаем её файлом размером 0, чтобы UI не падал.
       ESP_LOGW(
           TAG,
           "stat('%s') не удался",
           full_path
       );

       entry->is_dir = false;
       entry->size = 0;
   }


   count++;

  }

  closedir(dir);

  // Сортируем только реально найденные записи.
  qsort(
  out_entries,
  count,
  sizeof(fm_entry_t),
  compare_entries
  );

  *out_count = count;

  return ESP_OK;
  }

/**

* @brief Обновить кэш текущей директории.
*
* Вызывается автоматически после:
*
* ```
  входа в том;
  ```
* ```
  входа в папку;
  ```
* ```
  выхода из папки;
  ```
* ```
  создания;
  ```
* ```
  удаления;
  ```
* ```
  переименования.
  ```

*/
static void refresh_cache(void)
{
uint8_t active = __atomic_load_n(&s_active_cache_bank, __ATOMIC_RELAXED);
uint8_t staging_index = active ^ 1U;
fm_cache_bank_t *staging = &s_cache_banks[staging_index];
uint8_t count = 0;

esp_err_t err = list_current_dir_uncached(
    staging->entries,
    FM_BROWSER_MAX_ENTRIES,
    &count
);

if (err != ESP_OK) {

    // При ошибке не оставляем старый кэш, потому что он может
    // относиться уже к предыдущей директории.
    count = 0;

    ESP_LOGW(
        TAG,
        "Не удалось обновить кэш директории: %s",
        esp_err_to_name(err)
    );

}

staging->count = count;
strncpy(staging->path, s_current_path, sizeof(staging->path) - 1);
staging->path[sizeof(staging->path) - 1] = '\0';

__atomic_store_n(&s_active_cache_bank, staging_index, __ATOMIC_RELEASE);

}

// ============================================================================
// Публичный API кэша
// ============================================================================

uint8_t fm_get_cached_count(void)
{
fm_cache_snapshot_t snapshot;
fm_get_cache_snapshot(&snapshot);
return snapshot.count;
}

const fm_entry_t *fm_get_cached_entry(uint8_t index)
{
fm_cache_snapshot_t snapshot;
fm_get_cache_snapshot(&snapshot);
if (index >= snapshot.count) {
return NULL;
}

return &snapshot.entries[index];

}

void fm_get_cache_snapshot(fm_cache_snapshot_t *out_snapshot)
{
if (out_snapshot == NULL) {
return;
}

uint8_t active = __atomic_load_n(&s_active_cache_bank, __ATOMIC_ACQUIRE);
const fm_cache_bank_t *bank = &s_cache_banks[active];
out_snapshot->entries = bank->entries;
out_snapshot->path = bank->path;
out_snapshot->count = bank->count;
}

void fm_refresh_cache_snapshot(void)
{
refresh_cache();
}

// ============================================================================
// Навигация
// ============================================================================

esp_err_t fm_enter_volume(uint8_t volume_index)
{
if (volume_index >= s_volume_count) {
return ESP_ERR_INVALID_ARG;
}

const fm_volume_t *vol = s_volumes[volume_index];

if (vol == NULL || vol->mount_point == NULL) {
    return ESP_ERR_INVALID_STATE;
}


// Проверяем доступность тома.
//
// Для внутренней памяти callback обычно может быть:
//
//     storage_fat_is_mounted
//
// Для SD:
//
//     sd_card_is_mounted
//
if (
    vol->is_available != NULL &&
    !vol->is_available()
) {
    ESP_LOGW(
        TAG,
        "Том '%s' сейчас недоступен",
        vol->label
    );

    return ESP_ERR_INVALID_STATE;
}


// Сохраняем выбранный том.
s_current_volume = volume_index;
s_volume_selected = true;


// Текущий путь становится корнем выбранного тома.
strncpy(
    s_current_path,
    vol->mount_point,
    sizeof(s_current_path) - 1
);

s_current_path[sizeof(s_current_path) - 1] = '\0';


refresh_cache();

ESP_LOGI(
    TAG,
    "Вход в том '%s': %s",
    vol->label,
    s_current_path
);

return ESP_OK;

}

const char *fm_current_path(void)
{
// Если том ещё не выбран, возвращаем пустую строку.
//
// Это безопаснее, чем возвращать NULL:
// вызывающий UI может спокойно использовать строку.
if (!has_current_volume()) {
return "";
}

return s_current_path;


}

esp_err_t fm_enter_dir(const char *dir_name)
{
if (!has_current_volume()) {
return ESP_ERR_INVALID_STATE;
}
// Пользователь может входить только в папку по обычному имени,
// а не передавать произвольный путь.
if (!is_valid_name(dir_name)) {
    return ESP_ERR_INVALID_ARG;
}


char new_path[FM_MAX_PATH_LEN];

esp_err_t err = fm_build_full_path(
    dir_name,
    new_path,
    sizeof(new_path)
);

if (err != ESP_OK) {
    return err;
}


// Проверяем, что путь действительно существует.
struct stat st;

if (stat(new_path, &st) != 0) {
    ESP_LOGW(
        TAG,
        "Директория '%s' не существует",
        new_path
    );

    return ESP_ERR_NOT_FOUND;
}


// Проверяем, что это именно папка.
if (!S_ISDIR(st.st_mode)) {
    ESP_LOGW(
        TAG,
        "'%s' не является директорией",
        new_path
    );

    return ESP_ERR_INVALID_ARG;
}


// Только теперь меняем текущий путь.
strncpy(
    s_current_path,
    new_path,
    sizeof(s_current_path) - 1
);

s_current_path[sizeof(s_current_path) - 1] = '\0';


refresh_cache();

return ESP_OK;


}

bool fm_go_up(void)
{
if (!has_current_volume()) {
return false;
}

const fm_volume_t *vol = s_volumes[s_current_volume];


// Если уже в корне тома — выше идти нельзя.
if (strcmp(s_current_path, vol->mount_point) == 0) {
    return false;
}


// Ищем последний '/'.
//
// Например:
//
//     /storage/scripts/test
//                     ^
//                     |
//               последний slash
//
char *last_slash = strrchr(
    s_current_path,
    '/'
);

if (last_slash == NULL) {
    return false;
}


// Защита от попытки случайно обрезать путь до "/".
if (last_slash == s_current_path) {
    return false;
}


// Отрезаем последнюю часть пути.
*last_slash = '\0';


// Дополнительная защита:
// если по какой-то причине путь оказался короче mount_point,
// возвращаемся в корень тома.
if (strlen(s_current_path) < strlen(vol->mount_point)) {

    strncpy(
        s_current_path,
        vol->mount_point,
        sizeof(s_current_path) - 1
    );

    s_current_path[sizeof(s_current_path) - 1] = '\0';
}


refresh_cache();

return true;

}

esp_err_t fm_build_full_path(
const char *name,
char *out_path,
size_t out_path_size
)
{
if (
name == NULL ||
out_path == NULL ||
out_path_size == 0
) {
return ESP_ERR_INVALID_ARG;
}

if (!has_current_volume()) {
    return ESP_ERR_INVALID_STATE;
}


// Проверяем имя.
//
// fm_build_full_path используется и внутри файлового менеджера,
// поэтому защита находится здесь тоже, а не только в create/delete.
if (!is_valid_name(name)) {
    return ESP_ERR_INVALID_ARG;
}


int written = snprintf(
    out_path,
    out_path_size,
    "%s/%s",
    s_current_path,
    name
);


// snprintf возвращает количество символов, которое БЫЛО БЫ записано.
//
// Если результат >= размеру буфера — строка была обрезана.
if (
    written < 0 ||
    (size_t)written >= out_path_size
) {
    return ESP_ERR_INVALID_SIZE;
}


return ESP_OK;

}

// ============================================================================
// Операции с файлами и папками
// ============================================================================

static esp_err_t validate_directory_tree(const char *directory, uint8_t depth)
{
if (directory == NULL) {
    return ESP_ERR_INVALID_ARG;
}
if (depth >= FM_DELETE_MAX_DEPTH) {
    ESP_LOGE(TAG, "Слишком глубокая вложенность для удаления: '%s'", directory);
    return ESP_ERR_INVALID_SIZE;
}

DIR *dir = opendir(directory);
if (dir == NULL) {
    return ESP_FAIL;
}

esp_err_t err = ESP_OK;
while (true) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (entry == NULL) {
        if (errno != 0) err = ESP_FAIL;
        break;
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
        continue;
    }
    if (!is_valid_name(entry->d_name)) {
        err = ESP_ERR_INVALID_ARG;
        break;
    }

    char child_path[FM_MAX_PATH_LEN];
    int written = snprintf(child_path, sizeof(child_path), "%s/%s",
                           directory, entry->d_name);
    if (written < 0 || (size_t)written >= sizeof(child_path)) {
        err = ESP_ERR_INVALID_SIZE;
        break;
    }

    struct stat info;
    if (stat(child_path, &info) != 0) {
        err = ESP_FAIL;
        break;
    }
    if (S_ISDIR(info.st_mode)) {
        err = validate_directory_tree(child_path, depth + 1);
        if (err != ESP_OK) break;
    }
}

if (closedir(dir) != 0 && err == ESP_OK) {
    err = ESP_FAIL;
}
return err;
}

static esp_err_t delete_directory_tree(const char *directory, uint8_t depth)
{
if (directory == NULL) {
    return ESP_ERR_INVALID_ARG;
}

if (depth >= FM_DELETE_MAX_DEPTH) {
    ESP_LOGE(TAG, "Слишком глубокая вложенность при удалении: '%s'", directory);
    return ESP_ERR_INVALID_SIZE;
}

DIR *dir = opendir(directory);
if (dir == NULL) {
    ESP_LOGE(TAG, "Не удалось открыть удаляемую папку: '%s'", directory);
    return ESP_FAIL;
}

esp_err_t err = ESP_OK;
struct dirent *entry;

while (true) {
    errno = 0;
    entry = readdir(dir);
    if (entry == NULL) {
        if (errno != 0) {
            ESP_LOGE(TAG, "Ошибка чтения удаляемой папки: '%s'", directory);
            err = ESP_FAIL;
        }
        break;
    }

    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
        continue;
    }

    if (!is_valid_name(entry->d_name)) {
        ESP_LOGE(TAG, "Недопустимое имя внутри удаляемой папки: '%s'", entry->d_name);
        err = ESP_ERR_INVALID_ARG;
        break;
    }

    char child_path[FM_MAX_PATH_LEN];
    int written = snprintf(
        child_path,
        sizeof(child_path),
        "%s/%s",
        directory,
        entry->d_name
    );
    if (written < 0 || (size_t)written >= sizeof(child_path)) {
        err = ESP_ERR_INVALID_SIZE;
        break;
    }

    struct stat info;
    if (stat(child_path, &info) != 0) {
        ESP_LOGE(TAG, "Не удалось прочитать удаляемую запись: '%s'", child_path);
        err = ESP_FAIL;
        break;
    }

    if (S_ISDIR(info.st_mode)) {
        err = delete_directory_tree(child_path, depth + 1);
    } else if (remove(child_path) != 0) {
        ESP_LOGE(TAG, "Не удалось удалить файл: '%s'", child_path);
        err = ESP_FAIL;
    }

    if (err != ESP_OK) {
        break;
    }
}

if (closedir(dir) != 0 && err == ESP_OK) {
    ESP_LOGE(TAG, "Не удалось закрыть папку при удалении: '%s'", directory);
    err = ESP_FAIL;
}

if (err != ESP_OK) {
    return err;
}

if (rmdir(directory) != 0) {
    ESP_LOGE(TAG, "Не удалось удалить очищенную папку: '%s'", directory);
    return ESP_FAIL;
}

return ESP_OK;
}

esp_err_t fm_delete_entry(const char *name, bool is_dir)
{
if (!has_current_volume()) {
return ESP_ERR_INVALID_STATE;
}
  
if (!is_valid_name(name)) {
    return ESP_ERR_INVALID_ARG;
}


char full_path[FM_MAX_PATH_LEN];

esp_err_t err = fm_build_full_path(
    name,
    full_path,
    sizeof(full_path)
);

if (err != ESP_OK) {
    return err;
}


// Дополнительная проверка существования.
//
// Это позволяет вернуть понятную ошибку вместо попытки удалить
// несуществующий путь.
if (!path_exists(full_path)) {
    return ESP_ERR_NOT_FOUND;
}


if (is_dir) {
    err = validate_directory_tree(full_path, 0);
    if (err == ESP_OK) {
        err = delete_directory_tree(full_path, 0);
    }
} else {
    err = remove(full_path) == 0 ? ESP_OK : ESP_FAIL;
}


if (err != ESP_OK) {

    ESP_LOGE(
        TAG,
        "Не удалось удалить '%s' (is_dir=%d)",
        full_path,
        is_dir
    );

    return err;
}


ESP_LOGI(
    TAG,
    "Удалено: '%s'",
    full_path
);


refresh_cache();

return ESP_OK;

}

esp_err_t fm_create_dir(const char *name)
{
if (!has_current_volume()) {
return ESP_ERR_INVALID_STATE;
}
if (!is_valid_name(name)) {
    return ESP_ERR_INVALID_ARG;
}
if (has_internal_transaction_suffix(name)) {
    ESP_LOGW(TAG, "Имя зарезервировано для безопасного сохранения: '%s'", name);
    return ESP_ERR_INVALID_ARG;
}


char full_path[FM_MAX_PATH_LEN];

esp_err_t err = fm_build_full_path(
    name,
    full_path,
    sizeof(full_path)
);

if (err != ESP_OK) {
    return err;
}


// Не пытаемся создавать поверх существующей записи.
if (path_exists(full_path)) {

    ESP_LOGW(
        TAG,
        "Невозможно создать папку: '%s' уже существует",
        full_path
    );

    return ESP_ERR_INVALID_STATE;
}


if (mkdir(full_path, 0775) != 0) {

    ESP_LOGE(
        TAG,
        "Не удалось создать папку '%s'",
        full_path
    );

    return ESP_FAIL;
}


ESP_LOGI(
    TAG,
    "Создана папка '%s'",
    full_path
);


refresh_cache();

return ESP_OK;


}

esp_err_t fm_create_file(const char *name)
{
if (!has_current_volume()) {
return ESP_ERR_INVALID_STATE;
}


if (!is_valid_name(name)) {
    return ESP_ERR_INVALID_ARG;
}
if (has_internal_transaction_suffix(name)) {
    ESP_LOGW(TAG, "Имя зарезервировано для безопасного сохранения: '%s'", name);
    return ESP_ERR_INVALID_ARG;
}


char full_path[FM_MAX_PATH_LEN];

esp_err_t err = fm_build_full_path(
    name,
    full_path,
    sizeof(full_path)
);

if (err != ESP_OK) {
    return err;
}


// ВАЖНО:
//
// fopen(path, "w") уничтожает содержимое существующего файла.
//
// Поэтому сначала проверяем, существует ли запись.
if (path_exists(full_path)) {

    ESP_LOGW(
        TAG,
        "Невозможно создать файл: '%s' уже существует",
        full_path
    );

    return ESP_ERR_INVALID_STATE;
}


FILE *f = fopen(full_path, "w");

if (f == NULL) {

    ESP_LOGE(
        TAG,
        "Не удалось создать файл '%s'",
        full_path
    );

    return ESP_FAIL;
}


fclose(f);


ESP_LOGI(
    TAG,
    "Создан файл '%s'",
    full_path
);


refresh_cache();

return ESP_OK;

}

esp_err_t fm_rename(
const char *old_name,
const char *new_name
)
{
if (!has_current_volume()) {
return ESP_ERR_INVALID_STATE;
}

if (
    !is_valid_name(old_name) ||
    !is_valid_name(new_name)
) {
    return ESP_ERR_INVALID_ARG;
}
if (has_internal_transaction_suffix(new_name)) {
    ESP_LOGW(TAG, "Новое имя зарезервировано: '%s'", new_name);
    return ESP_ERR_INVALID_ARG;
}


// Нет смысла переименовывать запись в саму себя.
if (strcmp(old_name, new_name) == 0) {
    return ESP_OK;
}


char old_path[FM_MAX_PATH_LEN];
char new_path[FM_MAX_PATH_LEN];


esp_err_t err;


err = fm_build_full_path(
    old_name,
    old_path,
    sizeof(old_path)
);

if (err != ESP_OK) {
    return err;
}


err = fm_build_full_path(
    new_name,
    new_path,
    sizeof(new_path)
);

if (err != ESP_OK) {
    return err;
}


// Старое имя должно реально существовать.
if (!path_exists(old_path)) {
    return ESP_ERR_NOT_FOUND;
}


// Новое имя не должно уже существовать.
//
// Это предотвращает случайную потерю существующего файла.
if (path_exists(new_path)) {

    ESP_LOGW(
        TAG,
        "Переименование невозможно: '%s' уже существует",
        new_path
    );

    return ESP_ERR_INVALID_STATE;
}


if (rename(old_path, new_path) != 0) {

    ESP_LOGE(
        TAG,
        "Не удалось переименовать '%s' -> '%s'",
        old_path,
        new_path
    );

    return ESP_FAIL;
}


ESP_LOGI(
    TAG,
    "Переименовано: '%s' -> '%s'",
    old_path,
    new_path
);


refresh_cache();

return ESP_OK;

}
