#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
#define WIFI_FRAME_TYPE_MGMT 0x00
#define WIFI_FRAME_TYPE_CTRL 0x01
#define WIFI_FRAME_TYPE_DATA 0x02
#define WIFI_FRAME_SUBTYPE_BEACON 0x08
#define WIFI_FRAME_SUBTYPE_PROBE_REQ 0x04
#define WIFI_FRAME_SUBTYPE_PROBE_RESP 0x05
#define WIFI_FRAME_SUBTYPE_DEAUTH 0x0C
#define WIFI_FRAME_SUBTYPE_DISASSOC 0x0A

typedef struct {
    uint8_t type, subtype;
    uint8_t addr1[6], addr2[6], addr3[6]; /* Raw MAC header fields. */
    bool is_eapol; /* LLC EtherType only; NOT a validated/complete handshake. */
    int8_t rssi;
    uint8_t channel;
    uint8_t addr4[6], bssid[6];
    bool has_addr2, has_addr3, has_addr4, has_bssid;
    bool to_ds, from_ds, protected_frame, fragmented, qos, amsdu;
    uint16_t header_length;
    uint16_t eapol_offset; /* Valid only when is_eapol is true. */
    uint16_t eapol_length; /* Captured EAPOL bytes after LLC/SNAP. */
} wifi_frame_info_t;

typedef enum {
    WIFI_FRAME_DIRECTION_UNKNOWN = 0,
    WIFI_FRAME_DIRECTION_AP_TO_STA,
    WIFI_FRAME_DIRECTION_STA_TO_AP,
} wifi_frame_direction_t;

/* length = captured bytes excluding FCS, never the original on-air length.
 * Supports version-0 MGMT/DATA and classic ACK/CTS/RTS/PS-Poll/CF-End.
 * Other control formats are deliberately rejected. No FCS/body validation.
 * Unsupported encrypted/fragmented/aggregated payloads retain metadata but
 * never set is_eapol. out_info is zeroed on failure (if non-NULL).
 */
bool wifi_frame_parse(const uint8_t *payload, uint16_t length,
                     int8_t rssi, uint8_t channel, wifi_frame_info_t *out_info);
/* Returns roles only for unicast infrastructure DATA (exactly one DS bit). */
bool wifi_frame_get_infrastructure_roles(const wifi_frame_info_t *frame,
                                         uint8_t bssid[6], uint8_t sta[6],
                                         wifi_frame_direction_t *direction);
#ifdef __cplusplus
}
#endif
