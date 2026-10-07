#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    WIFI_PARSE_VALID = 0,
    WIFI_PARSE_PARTIAL,
    WIFI_PARSE_MALFORMED,
    WIFI_PARSE_UNSUPPORTED,
} wifi_parse_status_t;

typedef struct {
    const uint8_t *cursor;
    size_t remaining;
    wifi_parse_status_t status;
} wifi_ie_iterator_t;

typedef struct {
    uint8_t id;
    uint8_t length;
    const uint8_t *data; /* Borrowed from the MPDU; valid only during parsing. */
} wifi_ie_t;

void wifi_ie_iterator_init(wifi_ie_iterator_t *iterator,
                           const uint8_t *data, size_t length);
bool wifi_ie_iterator_next(wifi_ie_iterator_t *iterator, wifi_ie_t *element);
