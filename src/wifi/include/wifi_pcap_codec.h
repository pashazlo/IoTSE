#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_PCAP_GLOBAL_HEADER_LEN 24U
#define WIFI_PCAP_RECORD_HEADER_LEN 16U
#define WIFI_PCAP_LINKTYPE_RADIOTAP 127U
#define WIFI_PCAP_FULL_SNAPLEN      4091U
#define WIFI_RADIOTAP_MAX_LEN       16U
#define WIFI_PCAP_FILE_SNAPLEN      \
    (WIFI_PCAP_FULL_SNAPLEN + WIFI_RADIOTAP_MAX_LEN)

typedef enum {
    WIFI_PCAP_PHY_UNKNOWN = 0,
    WIFI_PCAP_PHY_LEGACY,
    WIFI_PCAP_PHY_HT,
} wifi_pcap_phy_t;

typedef struct {
    wifi_pcap_phy_t phy;
    uint8_t legacy_rate_code; /* ESP-IDF RX rate encoding, valid for LEGACY. */
    uint8_t mcs;
    bool bandwidth_40mhz;
    bool short_gi;
    int8_t rssi_dbm;
    uint8_t channel;
} wifi_pcap_radio_meta_t;

/* All serializers write explicit little-endian bytes and return bytes written,
 * or zero for invalid arguments/insufficient output capacity. */
size_t wifi_pcap_write_global_header(uint8_t *out, size_t capacity,
                                     uint32_t snaplen);
size_t wifi_pcap_write_record_header(uint8_t *out, size_t capacity,
                                     uint32_t seconds, uint32_t microseconds,
                                     uint32_t captured_length,
                                     uint32_t original_length);
size_t wifi_radiotap_write(uint8_t *out, size_t capacity,
                           const wifi_pcap_radio_meta_t *meta);

/* Select captured IEEE 802.11 bytes (FCS excluded). The ESP32-S3 sig_len field
 * is 12 bits including FCS, so at most 4091 MPDU bytes can be supplied. */
uint16_t wifi_pcap_select_frame_length(uint8_t frame_type, bool is_eapol,
                                       uint16_t available_length);

/* Converts ESP-IDF legacy RX rate encoding to Radiotap units of 500 kbit/s.
 * Returns zero when the code has no standard Radiotap RATE representation. */
uint8_t wifi_pcap_legacy_rate_500kbps(uint8_t esp_rate_code);

#ifdef __cplusplus
}
#endif
