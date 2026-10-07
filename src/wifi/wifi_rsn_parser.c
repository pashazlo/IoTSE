#include "wifi_rsn_parser.h"

#include <string.h>

static uint16_t le16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

static wifi_cipher_suite_t cipher(const uint8_t *p)
{
    wifi_cipher_suite_t s = {.kind = WIFI_CIPHER_UNKNOWN};
    memcpy(s.raw, p, 4U);
    if (p[0] != 0x00U || p[1] != 0x0fU || p[2] != 0xacU) return s;
    switch (p[3]) {
        case 0: s.kind = WIFI_CIPHER_NONE_OR_USE_GROUP; break;
        case 1: s.kind = WIFI_CIPHER_WEP40; break;
        case 2: s.kind = WIFI_CIPHER_TKIP; break;
        case 4: s.kind = WIFI_CIPHER_CCMP_128; break;
        case 5: s.kind = WIFI_CIPHER_WEP104; break;
        case 6: s.kind = WIFI_CIPHER_BIP_CMAC_128; break;
        case 8: s.kind = WIFI_CIPHER_GCMP_128; break;
        case 9: s.kind = WIFI_CIPHER_GCMP_256; break;
        case 10: s.kind = WIFI_CIPHER_CCMP_256; break;
        case 11: s.kind = WIFI_CIPHER_BIP_GMAC_128; break;
        case 12: s.kind = WIFI_CIPHER_BIP_GMAC_256; break;
        case 13: s.kind = WIFI_CIPHER_BIP_CMAC_256; break;
        default: break;
    }
    return s;
}

static wifi_akm_suite_t akm(const uint8_t *p)
{
    wifi_akm_suite_t s = {.kind = WIFI_AKM_UNKNOWN};
    memcpy(s.raw, p, 4U);
    if (p[0] != 0x00U || p[1] != 0x0fU || p[2] != 0xacU) return s;
    switch (p[3]) {
        case 1: s.kind = WIFI_AKM_8021X; break;
        case 2: s.kind = WIFI_AKM_PSK; break;
        case 3: s.kind = WIFI_AKM_FT_8021X; break;
        case 4: s.kind = WIFI_AKM_FT_PSK; break;
        case 5: s.kind = WIFI_AKM_8021X_SHA256; break;
        case 6: s.kind = WIFI_AKM_PSK_SHA256; break;
        case 8: s.kind = WIFI_AKM_SAE; break;
        case 9: s.kind = WIFI_AKM_FT_SAE; break;
        case 18: s.kind = WIFI_AKM_OWE; break;
        default: break;
    }
    return s;
}

wifi_parse_status_t wifi_rsn_parse(const uint8_t *data, size_t len,
                                   wifi_rsn_info_t *out)
{
    if (out == NULL) return WIFI_PARSE_MALFORMED;
    memset(out, 0, sizeof(*out));
    out->status = WIFI_PARSE_MALFORMED;
    out->pmf = WIFI_PMF_UNAVAILABLE;
    if (data == NULL || len < 8U) return out->status;
    size_t pos = 0U;
    out->version = le16(data); pos += 2U;
    if (out->version != 1U) { out->status = WIFI_PARSE_UNSUPPORTED; return out->status; }
    out->group_cipher = cipher(data + pos); pos += 4U;
    if (len - pos < 2U) return out->status;
    out->pairwise_advertised_count = le16(data + pos); pos += 2U;
    if (out->pairwise_advertised_count == 0U) return out->status;
    if ((size_t)out->pairwise_advertised_count > (len - pos) / 4U) return out->status;
    for (uint16_t i = 0; i < out->pairwise_advertised_count; ++i, pos += 4U) {
        if (out->pairwise_count < WIFI_RSN_MAX_PAIRWISE_CIPHERS)
            out->pairwise[out->pairwise_count++] = cipher(data + pos);
        else out->truncated = true;
    }
    if (len - pos < 2U) return out->status;
    out->akm_advertised_count = le16(data + pos); pos += 2U;
    if (out->akm_advertised_count == 0U) return out->status;
    if ((size_t)out->akm_advertised_count > (len - pos) / 4U) return out->status;
    for (uint16_t i = 0; i < out->akm_advertised_count; ++i, pos += 4U) {
        if (out->akm_count < WIFI_RSN_MAX_AKMS)
            out->akms[out->akm_count++] = akm(data + pos);
        else out->truncated = true;
    }
    if (pos == len) { out->status = WIFI_PARSE_VALID; return out->status; }
    if (len - pos < 2U) return out->status;
    out->has_capabilities = true;
    out->capabilities = le16(data + pos); pos += 2U;
    const bool capable = (out->capabilities & (1U << 7)) != 0U;
    const bool required = (out->capabilities & (1U << 6)) != 0U;
    out->pmf = required ? WIFI_PMF_REQUIRED : capable ? WIFI_PMF_CAPABLE : WIFI_PMF_DISABLED;
    if (pos == len) { out->status = WIFI_PARSE_VALID; return out->status; }
    if (len - pos < 2U) return out->status;
    out->has_pmkid_count = true;
    out->pmkid_count = le16(data + pos); pos += 2U;
    if ((size_t)out->pmkid_count > (len - pos) / 16U) return out->status;
    pos += (size_t)out->pmkid_count * 16U;
    if (pos == len) { out->status = WIFI_PARSE_VALID; return out->status; }
    if (len - pos != 4U) return out->status;
    out->has_group_management_cipher = true;
    out->group_management_cipher = cipher(data + pos);
    out->status = WIFI_PARSE_VALID;
    return out->status;
}
