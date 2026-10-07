#include <stdint.h>
#include <stdbool.h>

typedef enum {
    BLE_MODE_IDLE = 0,
    BLE_MODE_DISCOVERY,
    BLE_MODE_BEACON_IOS_TEST,
    BLE_MODE_BEACON_ANDROID_TEST,
    BLE_MODE_BEACON_WINDOWS_TEST,
    BLE_MODE_PERIPHERAL_EMULATION,
    BLE_MODE_REMOTE_INPUT_SIM,
    BLE_MODE_BRIDGE_DIAGNOSTIC
} ble_operation_mode_t;


typedef struct {
     ble_cmd_type_t type;
    uint32_t time_scan;




} ble_cmd_t;
