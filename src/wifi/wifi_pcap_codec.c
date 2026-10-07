#include "wifi_pcap_codec.h"

#include <string.h>

#include "wifi_frame_parser.h"

#define PCAP_MAGIC_USEC_LE       0xA1B2C3D4U
#define RADIOTAP_PRESENT_RATE    (1U << 2)
#define RADIOTAP_PRESENT_CHANNEL (1U << 3)
#define RADIOTAP_PRESENT_SIGNAL  (1U << 5)
#define RADIOTAP_PRESENT_MCS     (1U << 19)
#define RADIOTAP_CHAN_CCK        0x0020U
#define RADIOTAP_CHAN_OFDM       0x0040U
#define RADIOTAP_CHAN_2GHZ       0x0080U
#define RADIOTAP_MCS_HAVE_BW     0x01U
#define RADIOTAP_MCS_HAVE_INDEX  0x02U
#define RADIOTAP_MCS_HAVE_GI     0x04U
#define RADIOTAP_MCS_BW_40       0x01U
#define RADIOTAP_MCS_SHORT_GI    0x04U

static void put_le16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8U);
}

static void put_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8U);
    out[2] = (uint8_t)(value >> 16U);
    out[3] = (uint8_t)(value >> 24U);
}

static uint16_t channel_frequency_mhz(uint8_t channel)
{
    if (channel >= 1U && channel <= 13U) {
        return (uint16_t)(2407U + 5U * channel);
    }
    return channel == 14U ? 2484U : 0U;
}

uint8_t wifi_pcap_legacy_rate_500kbps(uint8_t code)
{
    static const uint8_t rates[16] = {
        2, 4, 11, 22, 0, 4, 11, 22,
        96, 48, 24, 12, 108, 72, 36, 18,
    };
    return code < sizeof(rates) ? rates[code] : 0U;
}

size_t wifi_pcap_write_global_header(uint8_t *out, size_t capacity,
                                     uint32_t snaplen)
{
    if (out == NULL || capacity < WIFI_PCAP_GLOBAL_HEADER_LEN || snaplen == 0U) {
        return 0U;
    }
    memset(out, 0, WIFI_PCAP_GLOBAL_HEADER_LEN);
    put_le32(out, PCAP_MAGIC_USEC_LE);
    put_le16(out + 4U, 2U);
    put_le16(out + 6U, 4U);
    put_le32(out + 16U, snaplen);
    put_le32(out + 20U, WIFI_PCAP_LINKTYPE_RADIOTAP);
    return WIFI_PCAP_GLOBAL_HEADER_LEN;
}

size_t wifi_pcap_write_record_header(uint8_t *out, size_t capacity,
                                     uint32_t seconds, uint32_t microseconds,
                                     uint32_t captured_length,
                                     uint32_t original_length)
{
    if (out == NULL || capacity < WIFI_PCAP_RECORD_HEADER_LEN ||
        microseconds >= 1000000U || captured_length > original_length) {
        return 0U;
    }
    put_le32(out, seconds);
    put_le32(out + 4U, microseconds);
    put_le32(out + 8U, captured_length);
    put_le32(out + 12U, original_length);
    return WIFI_PCAP_RECORD_HEADER_LEN;
}

size_t wifi_radiotap_write(uint8_t *out, size_t capacity,
                           const wifi_pcap_radio_meta_t *meta)
{
    if (out == NULL || meta == NULL || meta->channel < 1U ||
        meta->channel > 14U) {
        return 0U;
    }

    const uint16_t frequency = channel_frequency_mhz(meta->channel);
    uint32_t present = RADIOTAP_PRESENT_CHANNEL | RADIOTAP_PRESENT_SIGNAL;
    uint16_t channel_flags = RADIOTAP_CHAN_2GHZ;
    size_t length;

    if (meta->phy == WIFI_PCAP_PHY_LEGACY) {
        const uint8_t rate = wifi_pcap_legacy_rate_500kbps(
            meta->legacy_rate_code);
        if (rate == 0U || capacity < 15U) return 0U;
        present |= RADIOTAP_PRESENT_RATE;
        channel_flags |= rate <= 22U ? RADIOTAP_CHAN_CCK
                                     : RADIOTAP_CHAN_OFDM;
        memset(out, 0, 15U);
        put_le16(out + 2U, 15U);
        put_le32(out + 4U, present);
        out[8] = rate;
        /* offset 9 is alignment padding for the uint16 channel field. */
        put_le16(out + 10U, frequency);
        put_le16(out + 12U, channel_flags);
        out[14] = (uint8_t)meta->rssi_dbm;
        length = 15U;
    } else if (meta->phy == WIFI_PCAP_PHY_HT) {
        if (capacity < 16U || meta->mcs > 76U) return 0U;
        present |= RADIOTAP_PRESENT_MCS;
        channel_flags |= RADIOTAP_CHAN_OFDM;
        memset(out, 0, 16U);
        put_le16(out + 2U, 16U);
        put_le32(out + 4U, present);
        put_le16(out + 8U, frequency);
        put_le16(out + 10U, channel_flags);
        out[12] = (uint8_t)meta->rssi_dbm;
        out[13] = RADIOTAP_MCS_HAVE_BW | RADIOTAP_MCS_HAVE_INDEX |
                  RADIOTAP_MCS_HAVE_GI;
        out[14] = (meta->bandwidth_40mhz ? RADIOTAP_MCS_BW_40 : 0U) |
                  (meta->short_gi ? RADIOTAP_MCS_SHORT_GI : 0U);
        out[15] = meta->mcs;
        length = 16U;
    } else {
        if (capacity < 13U) return 0U;
        memset(out, 0, 13U);
        put_le16(out + 2U, 13U);
        put_le32(out + 4U, present);
        put_le16(out + 8U, frequency);
        put_le16(out + 10U, channel_flags);
        out[12] = (uint8_t)meta->rssi_dbm;
        length = 13U;
    }
    return length;
}

uint16_t wifi_pcap_select_frame_length(uint8_t frame_type, bool is_eapol,
                                       uint16_t available_length)
{
    if (available_length > WIFI_PCAP_FULL_SNAPLEN) {
        available_length = WIFI_PCAP_FULL_SNAPLEN;
    }
    (void)frame_type;
    (void)is_eapol;
    return available_length;
}
