#include "wifi_ie_iterator.h"

void wifi_ie_iterator_init(wifi_ie_iterator_t *it,
                           const uint8_t *data, size_t length)
{
    if (it == NULL) return;
    it->cursor = data;
    it->remaining = data != NULL ? length : 0U;
    it->status = data != NULL || length == 0U
        ? WIFI_PARSE_VALID : WIFI_PARSE_MALFORMED;
}

bool wifi_ie_iterator_next(wifi_ie_iterator_t *it, wifi_ie_t *element)
{
    if (it == NULL || element == NULL || it->status != WIFI_PARSE_VALID ||
        it->remaining == 0U) return false;
    if (it->remaining < 2U) {
        it->status = WIFI_PARSE_MALFORMED;
        return false;
    }
    const size_t element_length = it->cursor[1];
    if (element_length > it->remaining - 2U) {
        it->status = WIFI_PARSE_MALFORMED;
        return false;
    }
    element->id = it->cursor[0];
    element->length = (uint8_t)element_length;
    element->data = it->cursor + 2U;
    it->cursor += 2U + element_length;
    it->remaining -= 2U + element_length;
    return true;
}
