#include <assert.h>
#include <string.h>

#include "wifi_ie_iterator.h"
#include "wifi_mgmt_parser.h"
#include "wifi_rsn_parser.h"

static const uint8_t k_rsn[] = {
    1,0, 0,0x0f,0xac,4, 1,0, 0,0x0f,0xac,4,
    1,0, 0,0x0f,0xac,8, 0xc0,0, 0,0, 0,0x0f,0xac,6
};

static void test_ie_iterator_bounds(void)
{
    const uint8_t bad[] = {0, 4, 'a'};
    wifi_ie_iterator_t it;
    wifi_ie_t ie;
    wifi_ie_iterator_init(&it, bad, sizeof(bad));
    assert(!wifi_ie_iterator_next(&it, &ie));
    assert(it.status == WIFI_PARSE_MALFORMED);
}

static void test_rsn_and_every_prefix(void)
{
    wifi_rsn_info_t rsn;
    assert(wifi_rsn_parse(k_rsn, sizeof(k_rsn), &rsn) == WIFI_PARSE_VALID);
    assert(rsn.group_cipher.kind == WIFI_CIPHER_CCMP_128);
    assert(rsn.pairwise_count == 1 && rsn.pairwise[0].kind == WIFI_CIPHER_CCMP_128);
    assert(rsn.akm_count == 1 && rsn.akms[0].kind == WIFI_AKM_SAE);
    assert(rsn.pmf == WIFI_PMF_REQUIRED);
    assert(rsn.group_management_cipher.kind == WIFI_CIPHER_BIP_CMAC_128);
    for (size_t n = 0; n < sizeof(k_rsn); ++n) {
        wifi_parse_status_t status = wifi_rsn_parse(k_rsn, n, &rsn);
        assert(status == WIFI_PARSE_MALFORMED || status == WIFI_PARSE_VALID);
    }
}

static void test_bounded_rsn_lists(void)
{
    uint8_t data[2 + 4 + 2 + 9 * 4 + 2 + 4] = {1,0, 0,0x0f,0xac,4, 9,0};
    size_t p = 8;
    for (unsigned i = 0; i < 9; ++i) {
        const uint8_t suite[4] = {0,0x0f,0xac,4};
        memcpy(data + p, suite, 4); p += 4;
    }
    data[p++] = 1; data[p++] = 0;
    data[p++] = 0; data[p++] = 0x0f; data[p++] = 0xac; data[p++] = 2;
    wifi_rsn_info_t rsn;
    assert(wifi_rsn_parse(data, sizeof(data), &rsn) == WIFI_PARSE_VALID);
    assert(rsn.pairwise_advertised_count == 9);
    assert(rsn.pairwise_count == WIFI_RSN_MAX_PAIRWISE_CIPHERS);
    assert(rsn.truncated);
}

static void test_beacon_normalization(void)
{
    uint8_t mpdu[24 + 12 + 2 + 4 + 3 + 2 + sizeof(k_rsn)] = {0};
    size_t p = 24 + 12;
    mpdu[p++] = 0; mpdu[p++] = 4;
    memcpy(mpdu + p, "Lab!", 4); p += 4;
    mpdu[p++] = 3; mpdu[p++] = 1; mpdu[p++] = 6;
    mpdu[p++] = 48; mpdu[p++] = sizeof(k_rsn);
    memcpy(mpdu + p, k_rsn, sizeof(k_rsn));
    wifi_frame_info_t frame = {
        .type = WIFI_FRAME_TYPE_MGMT, .subtype = WIFI_FRAME_SUBTYPE_BEACON,
        .channel = 11, .header_length = 24,
    };
    wifi_mgmt_info_t mgmt;
    assert(wifi_mgmt_parse(mpdu, sizeof(mpdu), &frame, &mgmt) == WIFI_PARSE_VALID);
    assert(mgmt.rx_channel == 11 && mgmt.advertised_channel == 6);
    assert(mgmt.ssid_kind == WIFI_SSID_NORMAL && strcmp(mgmt.ssid, "Lab!") == 0);
    assert(mgmt.rsn_ie_seen && mgmt.rsn.status == WIFI_PARSE_VALID);
}

int main(void)
{
    test_ie_iterator_bounds();
    test_rsn_and_every_prefix();
    test_bounded_rsn_lists();
    test_beacon_normalization();
    return 0;
}
