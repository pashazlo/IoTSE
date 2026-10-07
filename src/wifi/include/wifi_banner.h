#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Converts untrusted network bytes into a bounded single-line preview.
 *
 * Rules:
 * - output is always NUL-terminated when output_capacity is non-zero;
 * - CR, LF, TAB and repeated spaces become one ordinary space;
 * - printable ASCII is copied as-is;
 * - binary/control bytes become '.' so terminal escapes cannot reach logs/UI;
 * - no allocation, logging, socket access or global state.
 *
 * Returns the number of visible output bytes, excluding the NUL terminator.
 */
size_t wifi_banner_sanitize(const uint8_t *input,
                            size_t input_length,
                            char *output,
                            size_t output_capacity,
                            bool *truncated);

#ifdef __cplusplus
}
#endif
