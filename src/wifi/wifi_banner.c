#include "wifi_banner.h"

size_t wifi_banner_sanitize(const uint8_t *input,
                            size_t input_length,
                            char *output,
                            size_t output_capacity,
                            bool *truncated)
{
    if (truncated != NULL) *truncated = false;
    if (output == NULL || output_capacity == 0U) {
        if (truncated != NULL) *truncated = input_length != 0U;
        return 0U;
    }

    output[0] = '\0';
    if (input == NULL || input_length == 0U) return 0U;

    size_t written = 0U;
    bool previous_was_space = true;
    for (size_t i = 0U; i < input_length; ++i) {
        const uint8_t byte = input[i];
        const bool whitespace = byte == ' ' || byte == '\r' ||
                                byte == '\n' || byte == '\t';
        char visible;

        if (whitespace) {
            if (previous_was_space) continue;
            visible = ' ';
        } else if (byte >= 0x20U && byte <= 0x7eU) {
            visible = (char)byte;
        } else {
            visible = '.';
        }

        if (written + 1U >= output_capacity) {
            if (truncated != NULL) *truncated = true;
            break;
        }
        output[written++] = visible;
        previous_was_space = visible == ' ';
    }

    /* A trailing separator carries no useful banner information. */
    if (written > 0U && output[written - 1U] == ' ') --written;
    output[written] = '\0';
    return written;
}
