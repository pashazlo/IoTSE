#include "subghz_rs_math.h"

#include <string.h>

static bool gf_ready(const subghz_rs_gf256_t *gf)
{
    return gf != NULL && gf->initialized;
}

static uint8_t gf_mul_without_tables(uint8_t a,
                                     uint8_t b,
                                     uint16_t primitive_polynomial)
{
    uint8_t result = 0U;
    while (b != 0U) {
        if ((b & 1U) != 0U) result ^= a;
        b >>= 1U;

        const bool carry = (a & 0x80U) != 0U;
        a <<= 1U;
        if (carry) a ^= (uint8_t)(primitive_polynomial & 0xffU);
    }
    return result;
}

bool subghz_rs_gf256_init(subghz_rs_gf256_t *gf,
                          uint16_t primitive_polynomial,
                          uint8_t generator)
{
    if (gf == NULL || primitive_polynomial < 0x100U || generator < 2U) {
        return false;
    }

    memset(gf, 0, sizeof(*gf));
    gf->primitive_polynomial = primitive_polynomial;
    gf->generator = generator;

    uint8_t value = 1U;
    for (size_t i = 0; i < SUBGHZ_RS_NONZERO_ELEMENTS; ++i) {
        /* Repeating early means generator is not primitive for this field. */
        if (i != 0U && value == 1U) return false;
        gf->exp[i] = value;
        gf->log[value] = (uint8_t)i;
        value = gf_mul_without_tables(value, generator, primitive_polynomial);
    }
    if (value != 1U) return false;

    /* Duplicating exp removes a modulo operation from multiplication. */
    for (size_t i = SUBGHZ_RS_NONZERO_ELEMENTS;
         i < sizeof(gf->exp);
         ++i) {
        gf->exp[i] = gf->exp[i - SUBGHZ_RS_NONZERO_ELEMENTS];
    }
    gf->initialized = true;
    return true;
}

uint8_t subghz_rs_gf_add(uint8_t a, uint8_t b)
{
    return a ^ b;
}

uint8_t subghz_rs_gf_mul(const subghz_rs_gf256_t *gf, uint8_t a, uint8_t b)
{
    if (!gf_ready(gf) || a == 0U || b == 0U) return 0U;
    return gf->exp[(size_t)gf->log[a] + gf->log[b]];
}

uint8_t subghz_rs_gf_div(const subghz_rs_gf256_t *gf, uint8_t a, uint8_t b)
{
    if (!gf_ready(gf) || a == 0U || b == 0U) return 0U;
    int exponent = (int)gf->log[a] - (int)gf->log[b];
    if (exponent < 0) exponent += (int)SUBGHZ_RS_NONZERO_ELEMENTS;
    return gf->exp[exponent];
}

uint8_t subghz_rs_gf_pow(const subghz_rs_gf256_t *gf, uint8_t a, int exponent)
{
    if (!gf_ready(gf)) return 0U;
    if (exponent == 0) return 1U;
    if (a == 0U) return 0U;

    int reduced = ((int)gf->log[a] * exponent) %
                  (int)SUBGHZ_RS_NONZERO_ELEMENTS;
    if (reduced < 0) reduced += (int)SUBGHZ_RS_NONZERO_ELEMENTS;
    return gf->exp[reduced];
}

uint8_t subghz_rs_poly_eval(const subghz_rs_gf256_t *gf,
                            const uint8_t *polynomial,
                            size_t coefficient_count,
                            uint8_t x)
{
    if (!gf_ready(gf) || polynomial == NULL || coefficient_count == 0U) {
        return 0U;
    }

    uint8_t result = polynomial[coefficient_count - 1U];
    for (size_t i = coefficient_count - 1U; i > 0U; --i) {
        result = subghz_rs_gf_mul(gf, result, x) ^ polynomial[i - 1U];
    }
    return result;
}

bool subghz_rs_compute_syndromes(const subghz_rs_gf256_t *gf,
                                 const uint8_t *codeword,
                                 size_t codeword_length,
                                 uint8_t first_root,
                                 uint8_t *syndromes,
                                 size_t syndrome_count)
{
    if (!gf_ready(gf) || codeword == NULL || codeword_length == 0U ||
        codeword_length > SUBGHZ_RS_NONZERO_ELEMENTS || syndromes == NULL ||
        syndrome_count == 0U || syndrome_count > SUBGHZ_RS_MAX_PARITY) {
        return false;
    }

    for (size_t j = 0; j < syndrome_count; ++j) {
        const uint8_t x = gf->exp[((size_t)first_root + j) %
                                  SUBGHZ_RS_NONZERO_ELEMENTS];
        uint8_t value = 0U;
        /* codeword[] is high-degree-first, hence ordinary Horner order. */
        for (size_t i = 0; i < codeword_length; ++i) {
            value = subghz_rs_gf_mul(gf, value, x) ^ codeword[i];
        }
        syndromes[j] = value;
    }
    return true;
}

bool subghz_rs_find_error_locator(const subghz_rs_gf256_t *gf,
                                  const uint8_t *syndromes,
                                  size_t syndrome_count,
                                  uint8_t *locator,
                                  size_t locator_capacity,
                                  size_t *locator_degree)
{
    if (!gf_ready(gf) || syndromes == NULL || syndrome_count == 0U ||
        syndrome_count > SUBGHZ_RS_MAX_PARITY || locator == NULL ||
        locator_capacity < syndrome_count + 1U || locator_degree == NULL) {
        return false;
    }

    uint8_t previous[SUBGHZ_RS_MAX_PARITY + 1U] = {0};
    uint8_t saved[SUBGHZ_RS_MAX_PARITY + 1U] = {0};
    memset(locator, 0, locator_capacity);
    locator[0] = 1U;
    previous[0] = 1U;

    size_t degree = 0U;
    size_t shift = 1U;
    uint8_t previous_discrepancy = 1U;

    for (size_t n = 0; n < syndrome_count; ++n) {
        uint8_t discrepancy = syndromes[n];
        for (size_t i = 1U; i <= degree; ++i) {
            discrepancy ^= subghz_rs_gf_mul(gf, locator[i], syndromes[n - i]);
        }

        if (discrepancy == 0U) {
            ++shift;
            continue;
        }

        memcpy(saved, locator, syndrome_count + 1U);
        const uint8_t scale = subghz_rs_gf_div(
            gf, discrepancy, previous_discrepancy);
        for (size_t i = 0U; i + shift <= syndrome_count; ++i) {
            locator[i + shift] ^=
                subghz_rs_gf_mul(gf, scale, previous[i]);
        }

        if ((2U * degree) <= n) {
            degree = n + 1U - degree;
            memcpy(previous, saved, syndrome_count + 1U);
            previous_discrepancy = discrepancy;
            shift = 1U;
        } else {
            ++shift;
        }
    }

    /* r parity symbols can correct at most floor(r/2) unknown errors. */
    if ((2U * degree) > syndrome_count) return false;
    *locator_degree = degree;
    return true;
}

bool subghz_rs_compute_error_evaluator(const subghz_rs_gf256_t *gf,
                                       const uint8_t *syndromes,
                                       size_t syndrome_count,
                                       const uint8_t *locator,
                                       size_t locator_degree,
                                       uint8_t *evaluator,
                                       size_t evaluator_capacity)
{
    if (!gf_ready(gf) || syndromes == NULL || locator == NULL ||
        evaluator == NULL || syndrome_count == 0U ||
        syndrome_count > SUBGHZ_RS_MAX_PARITY ||
        locator_degree > syndrome_count || evaluator_capacity < syndrome_count) {
        return false;
    }

    memset(evaluator, 0, evaluator_capacity);
    for (size_t i = 0; i <= locator_degree; ++i) {
        for (size_t j = 0; j < syndrome_count && i + j < syndrome_count; ++j) {
            evaluator[i + j] ^=
                subghz_rs_gf_mul(gf, locator[i], syndromes[j]);
        }
    }
    return true;
}

bool subghz_rs_chien_search(const subghz_rs_gf256_t *gf,
                            const uint8_t *locator,
                            size_t locator_degree,
                            size_t codeword_length,
                            size_t *error_positions,
                            size_t error_positions_capacity,
                            size_t *error_count)
{
    if (!gf_ready(gf) || locator == NULL || locator_degree == 0U ||
        codeword_length == 0U || codeword_length > SUBGHZ_RS_NONZERO_ELEMENTS ||
        error_positions == NULL || error_positions_capacity < locator_degree ||
        error_count == NULL) {
        return false;
    }

    size_t found = 0U;
    for (size_t position = 0; position < codeword_length; ++position) {
        const size_t location_number = codeword_length - 1U - position;
        const uint8_t inverse_location = gf->exp[
            (SUBGHZ_RS_NONZERO_ELEMENTS -
             (location_number % SUBGHZ_RS_NONZERO_ELEMENTS)) %
            SUBGHZ_RS_NONZERO_ELEMENTS];
        if (subghz_rs_poly_eval(
                gf, locator, locator_degree + 1U, inverse_location) == 0U) {
            if (found >= error_positions_capacity) return false;
            error_positions[found++] = position;
        }
    }

    /* A locator of degree L is trustworthy only when exactly L roots exist. */
    if (found != locator_degree) return false;
    *error_count = found;
    return true;
}

bool subghz_rs_forney_magnitude(const subghz_rs_gf256_t *gf,
                                const uint8_t *locator,
                                size_t locator_degree,
                                const uint8_t *evaluator,
                                size_t evaluator_count,
                                size_t codeword_length,
                                size_t error_position,
                                uint8_t first_root,
                                uint8_t *magnitude)
{
    if (!gf_ready(gf) || locator == NULL || locator_degree == 0U ||
        evaluator == NULL || evaluator_count == 0U || codeword_length == 0U ||
        codeword_length > SUBGHZ_RS_NONZERO_ELEMENTS ||
        error_position >= codeword_length || magnitude == NULL) {
        return false;
    }

    const size_t location_number = codeword_length - 1U - error_position;
    const uint8_t location = gf->exp[
        location_number % SUBGHZ_RS_NONZERO_ELEMENTS];
    const uint8_t inverse_location = gf->exp[
        (SUBGHZ_RS_NONZERO_ELEMENTS -
         (location_number % SUBGHZ_RS_NONZERO_ELEMENTS)) %
        SUBGHZ_RS_NONZERO_ELEMENTS];

    const uint8_t numerator = subghz_rs_poly_eval(
        gf, evaluator, evaluator_count, inverse_location);

    /*
     * In characteristic two, d/dx removes even powers and maps
     * lambda_i*x^i to lambda_i*x^(i-1) only for odd i.
     */
    uint8_t derivative = 0U;
    uint8_t x_power = 1U;
    for (size_t i = 1U; i <= locator_degree; ++i) {
        if ((i & 1U) != 0U) derivative ^=
            subghz_rs_gf_mul(gf, locator[i], x_power);
        x_power = subghz_rs_gf_mul(gf, x_power, inverse_location);
    }
    if (derivative == 0U) return false;

    const uint8_t root_factor = subghz_rs_gf_pow(
        gf, location, 1 - (int)first_root);
    *magnitude = subghz_rs_gf_mul(
        gf, root_factor, subghz_rs_gf_div(gf, numerator, derivative));
    return true;
}
