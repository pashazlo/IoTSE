#include "fm_text_view.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "fm_text_view";

// ============================================================================
// Индекс визуальной строки
// ============================================================================
//
// Сам текст НЕ копируется.
//
// Например, если:
//
// s_buffer = "Hello world\nThis is text"
//
// то строка может храниться как:
//
// offset = 0
// length = 11
//
// Это позволяет одному буферу содержать весь файл, а массиву s_lines —
// только описывать, где начинается и заканчивается каждая визуальная
// строка.
// ============================================================================

typedef struct {
uint32_t offset;
uint16_t length;
} fm_text_line_t;

// ============================================================================
// Состояние просмотрщика
// ============================================================================

static char *s_buffer = NULL;
static size_t s_buffer_len = 0;

static fm_text_line_t s_lines[FM_TEXT_MAX_LINES];
static uint16_t s_line_count = 0;

static uint16_t s_scroll_top = 0;

static bool s_truncated = false;

// ============================================================================
// Поиск точки переноса
// ============================================================================

static size_t find_wrap_point(
const gfx_font_t *font,
int16_t wrap_width_px,
size_t start,
size_t limit
)
{
int16_t width = 0;

size_t last_space = start;
bool have_space = false;


for (size_t i = start; i < limit; i++) {

    uint8_t ch = (uint8_t)s_buffer[i];

    int16_t adv = 0;


    // Если символ есть в текущем шрифте — берём его реальную ширину.
    //
    // Если символа нет — считаем его нулевой ширины.
    // Это сохраняет текущую архитектуру gfx_font.
    if (ch >= font->first && ch <= font->last) {
        adv = font->glyphs[ch - font->first].xAdvance;
    }


    // Следующий символ уже не помещается.
    if (width + adv > wrap_width_px) {

        // Если раньше встретили пробел — переносимся по нему.
        if (have_space && last_space > start) {
            return last_space;
        }


        // Слово само по себе длиннее экрана.
        //
        // Жёстко переносим хотя бы после одного символа.
        //
        // Это важно: функция ВСЕГДА должна продвигаться вперёд,
        // иначе можно получить бесконечный цикл в word-wrap.
        return (i > start) ? i : i + 1;
    }


    width += adv;


    if (ch == ' ') {
        have_space = true;
        last_space = i;
    }
}


// Весь остаток диапазона помещается.
return limit;

}

// ============================================================================
// Добавление визуальной строки
// ============================================================================

static void add_line(uint32_t offset, size_t length)
{
if (s_line_count >= FM_TEXT_MAX_LINES) {
s_truncated = true;
return;
}

// fm_text_line_t.length имеет тип uint16_t.
//
// Обычно визуальная строка намного короче 65535 байт, но всё равно
// не позволяем переполнить поле.
if (length > UINT16_MAX) {
    length = UINT16_MAX;
    s_truncated = true;
}


s_lines[s_line_count].offset = offset;
s_lines[s_line_count].length = (uint16_t)length;

s_line_count++;

}

// ============================================================================
// Word-wrap и построение индекса
// ============================================================================

static void wrap_and_index(
const gfx_font_t *font,
int16_t wrap_width_px
)
{
s_line_count = 0;
s_truncated = false;

// Пустой файл тоже должен иметь одну визуальную строку.
//
// Это делает поведение просмотрщика предсказуемым:
//
//     line_count() == 1
//
// вместо странного "файл открыт, но строк 0".
if (s_buffer_len == 0) {
    add_line(0, 0);
    return;
}


size_t pos = 0;


while (pos < s_buffer_len &&
       s_line_count < FM_TEXT_MAX_LINES) {

    size_t raw_start = pos;
    size_t raw_end = raw_start;


    // Ищем настоящий '\n' в исходном файле.
    while (raw_end < s_buffer_len &&
           s_buffer[raw_end] != '\n') {

        raw_end++;
    }


    // --------------------------------------------------------------------
    // Пустая исходная строка.
    // --------------------------------------------------------------------

    if (raw_end == raw_start) {

        add_line((uint32_t)raw_start, 0);
    }

    else {

        size_t seg_start = raw_start;


        while (seg_start < raw_end &&
               s_line_count < FM_TEXT_MAX_LINES) {

            size_t seg_end = find_wrap_point(
                font,
                wrap_width_px,
                seg_start,
                raw_end
            );


            // Защита от некорректного результата word-wrap.
            if (seg_end <= seg_start) {
                seg_end = seg_start + 1;
            }

            if (seg_end > raw_end) {
                seg_end = raw_end;
            }


            // Если перед '\n' стоит '\r', это Windows-окончание
            // строки "\r\n". Не показываем '\r' как символ текста.
            size_t visible_end = seg_end;

            if (visible_end > seg_start &&
                s_buffer[visible_end - 1] == '\r') {

                visible_end--;
            }


            add_line(
                (uint32_t)seg_start,
                visible_end - seg_start
            );


            seg_start = seg_end;


            // Если перенос произошёл по пробелу, пробел не должен
            // становиться первым символом следующей визуальной строки.
            if (seg_start < raw_end &&
                s_buffer[seg_start] == ' ') {

                seg_start++;
            }
        }
    }


    // Переходим к следующей исходной строке.
    //
    // Если raw_end == s_buffer_len, мы уже дошли до конца файла.
    pos = (raw_end < s_buffer_len)
        ? raw_end + 1
        : raw_end;
}


// Если остался необработанный текст — упёрлись в лимит строк.
if (pos < s_buffer_len) {
    s_truncated = true;
}

}

// ============================================================================
// Открытие файла
// ============================================================================

esp_err_t fm_text_view_open(
const char *full_path,
const gfx_font_t *font,
int16_t wrap_width_px
)
{
if (full_path == NULL ||
font == NULL ||
wrap_width_px <= 0) {

    return ESP_ERR_INVALID_ARG;
}


// Предыдущий документ освобождаем автоматически.
fm_text_view_close();


FILE *f = fopen(full_path, "rb");

if (f == NULL) {

    ESP_LOGE(TAG,
             "Не удалось открыть '%s'",
             full_path);

    return ESP_ERR_NOT_FOUND;
}


// ------------------------------------------------------------------------
// Определяем размер файла.
// ------------------------------------------------------------------------

if (fseek(f, 0, SEEK_END) != 0) {

    fclose(f);

    ESP_LOGE(TAG,
             "fseek('%s') не удался",
             full_path);

    return ESP_FAIL;
}


long file_size = ftell(f);


if (file_size < 0) {

    fclose(f);

    ESP_LOGE(TAG,
             "ftell('%s') не удался",
             full_path);

    return ESP_FAIL;
}


if (fseek(f, 0, SEEK_SET) != 0) {

    fclose(f);

    ESP_LOGE(TAG,
             "Не удалось вернуться в начало '%s'",
             full_path);

    return ESP_FAIL;
}


size_t read_size = (size_t)file_size;

bool truncated_by_size = false;


// ------------------------------------------------------------------------
// Ограничиваем максимальный размер загружаемого файла.
// ------------------------------------------------------------------------

if (read_size > FM_TEXT_MAX_FILE_SIZE) {

    read_size = FM_TEXT_MAX_FILE_SIZE;
    truncated_by_size = true;


    ESP_LOGW(
        TAG,
        "Файл '%s' (%ld байт) больше лимита %d — показываем начало",
        full_path,
        file_size,
        FM_TEXT_MAX_FILE_SIZE
    );
}


// ------------------------------------------------------------------------
// malloc(0) нельзя считать надёжным способом получить рабочий буфер.
//
// Поэтому даже для пустого файла выделяем 1 байт.
// ------------------------------------------------------------------------

size_t alloc_size = (read_size == 0)
    ? 1
    : read_size;


// Стараемся разместить большой буфер именно в PSRAM.
s_buffer = heap_caps_malloc(
    alloc_size,
    MALLOC_CAP_SPIRAM
);


if (s_buffer == NULL) {

    fclose(f);

    ESP_LOGE(
        TAG,
        "Не хватило PSRAM под буфер файла (%zu байт)",
        alloc_size
    );

    return ESP_ERR_NO_MEM;
}


// ------------------------------------------------------------------------
// Читаем файл.
// ------------------------------------------------------------------------

size_t actually_read = 0;

if (read_size > 0) {

    actually_read = fread(
        s_buffer,
        1,
        read_size,
        f
    );


    // fread() может вернуть меньше данных не только из-за EOF,
    // но и из-за ошибки самого файла/VFS.
    if (actually_read < read_size &&
        ferror(f)) {

        free(s_buffer);
        s_buffer = NULL;

        fclose(f);

        ESP_LOGE(
            TAG,
            "Ошибка чтения '%s'",
            full_path
        );

        return ESP_FAIL;
    }
}


fclose(f);


s_buffer_len = actually_read;


// ------------------------------------------------------------------------
// Если файл прочитан не полностью, но ошибки нет — это обычно EOF.
// В таком случае показываем реально прочитанную часть.
// ------------------------------------------------------------------------

if (actually_read < read_size) {
    truncated_by_size = true;
}


// ------------------------------------------------------------------------
// Строим индекс визуальных строк.
// ------------------------------------------------------------------------

wrap_and_index(
    font,
    wrap_width_px
);


if (truncated_by_size) {
    s_truncated = true;
}


s_scroll_top = 0;


ESP_LOGI(
    TAG,
    "Открыт '%s': %zu байт, %u строк%s",
    full_path,
    s_buffer_len,
    s_line_count,
    s_truncated ? " (обрезано)" : ""
);


return ESP_OK;

}

// ============================================================================
// Закрытие
// ============================================================================

void fm_text_view_close(void)
{
if (s_buffer != NULL) {

    free(s_buffer);
    s_buffer = NULL;
}


s_buffer_len = 0;
s_line_count = 0;
s_scroll_top = 0;
s_truncated = false;

}

// ============================================================================
// Прокрутка
// ============================================================================

void fm_text_view_scroll(
int16_t delta_lines,
uint8_t visible_lines
)
{
int32_t new_top =
(int32_t)s_scroll_top + delta_lines;

int32_t max_top =
    (s_line_count > visible_lines)
    ? (int32_t)(s_line_count - visible_lines)
    : 0;


if (new_top < 0) {
    new_top = 0;
}


if (new_top > max_top) {
    new_top = max_top;
}


s_scroll_top = (uint16_t)new_top;

}

// ============================================================================
// Отрисовка
// ============================================================================

void fm_text_view_draw(
gfx_canvas_t *canvas,
const gfx_font_t *font,
int16_t x,
int16_t y,
int16_t line_height,
uint8_t visible_lines,
uint16_t color
)
{
if (canvas == NULL ||
font == NULL ||
s_buffer == NULL ||
line_height <= 0) {

    return;
}


for (uint8_t row = 0;
     row < visible_lines;
     row++) {

    uint16_t line_idx =
        s_scroll_top + row;


    if (line_idx >= s_line_count) {
        break;
    }


    const fm_text_line_t *line =
        &s_lines[line_idx];


    // --------------------------------------------------------------------
    // gfx_canvas_draw_str() принимает обычную C-строку.
    //
    // Внутренние строки просмотрщика НЕ null-terminated, поэтому
    // делаем временную копию.
    //
    // 256 байт — запас относительно обычной ширины строки на
    // небольшом дисплее.
    // --------------------------------------------------------------------

    char line_buf[256];


    uint16_t copy_len = line->length;


    if (copy_len >= sizeof(line_buf)) {
        copy_len = sizeof(line_buf) - 1;
    }


    memcpy(
        line_buf,
        &s_buffer[line->offset],
        copy_len
    );


    line_buf[copy_len] = '\0';


    gfx_canvas_draw_str(
        canvas,
        x,
        y + row * line_height,
        line_buf,
        font,
        color
    );
}

}

// ============================================================================
// Информация
// ============================================================================

uint16_t fm_text_view_line_count(void)
{
return s_line_count;
}

bool fm_text_view_is_truncated(void)
{
return s_truncated;
}
