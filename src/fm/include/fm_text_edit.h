#ifndef FM_TEXT_EDIT_H
#define FM_TEXT_EDIT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define FM_EDIT_MAX_LINES    100
#define FM_EDIT_MAX_LINE_LEN 64

// --- Инициализация и системные функции ---
void fm_text_edit_init(void);
void fm_text_edit_clear(void);
void fm_text_edit_close(void);

// --- Загрузка / Сохранение ---
bool fm_text_edit_open(const char *filepath);
bool fm_text_edit_save(void);
bool fm_text_edit_save_as(const char *filepath);
const char *fm_text_edit_get_filepath(void);

// --- Геттеры состояния модели ---
uint16_t fm_text_edit_line_count(void);
const char *fm_text_edit_get_line(uint16_t line_index);
uint16_t fm_text_edit_line_length(uint16_t line_index);

// --- Модификация документа ---
bool fm_text_edit_set_line(uint16_t line_index, const char *text);
bool fm_text_edit_ensure_line(uint16_t line_index);
bool fm_text_edit_insert_line_after(uint16_t line_index);
bool fm_text_edit_delete_line(uint16_t line_index);

// --- Операции над текстом (Line + Column) ---
bool fm_text_edit_insert_text(uint16_t line, uint16_t column, const char *text);
bool fm_text_edit_delete_char(uint16_t line, uint16_t column);
bool fm_text_edit_backspace(uint16_t line, uint16_t column);

#endif // FM_TEXT_EDIT_H
