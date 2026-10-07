#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Small, allocation-free Reed-Solomon math layer for the future Sub-GHz
 * decoder. This module knows nothing about CC1101, RMT, packets, CRC or UI.
 *
 * Polynomial convention used by every function in this file:
 *   polynomial[i] is the coefficient of x^i (least-significant first).
 *
 * The context owns its lookup tables. The caller allocates it, normally in
 * static/internal RAM, initializes it once, and keeps it immutable afterward.
 * Sharing one initialized context between tasks is safe because calculations
 * only read it.
 */
#define SUBGHZ_RS_FIELD_SIZE 256U
#define SUBGHZ_RS_NONZERO_ELEMENTS 255U
#define SUBGHZ_RS_MAX_PARITY 64U

typedef struct {
    uint16_t primitive_polynomial;
    uint8_t generator;
    uint8_t exp[SUBGHZ_RS_NONZERO_ELEMENTS * 2U];
    uint8_t log[SUBGHZ_RS_FIELD_SIZE];
    bool initialized;
} subghz_rs_gf256_t;

/*
 * Builds GF(2^8) logarithm/exponent tables.
 *
 * Common RS(255, k) parameters are primitive_polynomial=0x11d and
 * generator=0x02, but a protocol descriptor must provide the real values.
 * The function rejects a generator that does not visit all 255 non-zero
 * field elements.
 */
bool subghz_rs_gf256_init(subghz_rs_gf256_t *gf,
                          uint16_t primitive_polynomial,
                          uint8_t generator);

uint8_t subghz_rs_gf_add(uint8_t a, uint8_t b);
uint8_t subghz_rs_gf_mul(const subghz_rs_gf256_t *gf, uint8_t a, uint8_t b);
uint8_t subghz_rs_gf_div(const subghz_rs_gf256_t *gf, uint8_t a, uint8_t b);
uint8_t subghz_rs_gf_pow(const subghz_rs_gf256_t *gf, uint8_t a, int exponent);

/* Evaluates a low-degree-first polynomial at x using Horner's method. */
uint8_t subghz_rs_poly_eval(const subghz_rs_gf256_t *gf,
                            const uint8_t *polynomial,
                            size_t coefficient_count,
                            uint8_t x);

/*
 * Computes S_j = R(alpha^(first_root + j)).
 *
 * codeword[0] is the leftmost/highest-degree symbol of R(x). Syndromes are
 * returned as [S_0, S_1, ...]. All-zero syndromes mean only that the word is
 * a valid member of this RS code; CRC and protocol validation remain separate.
 */
bool subghz_rs_compute_syndromes(const subghz_rs_gf256_t *gf,
                                 const uint8_t *codeword,
                                 size_t codeword_length,
                                 uint8_t first_root,
                                 uint8_t *syndromes,
                                 size_t syndrome_count);

/*
 * Berlekamp-Massey solves the key equation for errors-only input:
 *
 *   Lambda(x) * S(x) = Omega(x) mod x^r
 *
 * and returns Lambda(x) = 1 + lambda_1*x + ... + lambda_L*x^L.
 * Erasure locations are intentionally deferred to the frame-fusion layer.
 */
bool subghz_rs_find_error_locator(const subghz_rs_gf256_t *gf,
                                  const uint8_t *syndromes,
                                  size_t syndrome_count,
                                  uint8_t *locator,
                                  size_t locator_capacity,
                                  size_t *locator_degree);

/* Computes Omega(x) = [S(x) * Lambda(x)] mod x^r. */
bool subghz_rs_compute_error_evaluator(const subghz_rs_gf256_t *gf,
                                       const uint8_t *syndromes,
                                       size_t syndrome_count,
                                       const uint8_t *locator,
                                       size_t locator_degree,
                                       uint8_t *evaluator,
                                       size_t evaluator_capacity);

/*
 * Chien search checks Lambda(X^-1)=0 for each symbol location.
 * Returned positions use the same left-to-right indexing as codeword[].
 */
bool subghz_rs_chien_search(const subghz_rs_gf256_t *gf,
                            const uint8_t *locator,
                            size_t locator_degree,
                            size_t codeword_length,
                            size_t *error_positions,
                            size_t error_positions_capacity,
                            size_t *error_count);

/*
 * Forney magnitude for one location:
 *
 *   E_i = X_i^(1-b) * Omega(X_i^-1) / Lambda'(X_i^-1)
 *
 * where b is first_root and X_i=alpha^(n-1-position). In GF(2^m), subtraction
 * and addition are both XOR, so the usual leading minus sign has no effect.
 */
bool subghz_rs_forney_magnitude(const subghz_rs_gf256_t *gf,
                                const uint8_t *locator,
                                size_t locator_degree,
                                const uint8_t *evaluator,
                                size_t evaluator_count,
                                size_t codeword_length,
                                size_t error_position,
                                uint8_t first_root,
                                uint8_t *magnitude);

#ifdef __cplusplus
}
#endif
