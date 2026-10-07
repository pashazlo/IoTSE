# Wi-Fi connection crash remediation

## ROOT CAUSE

**CONFIRMED — SOURCE PROOF + HARDWARE BACKTRACE**

`start_attempt()` used:

```c
ESP_LOGI(TAG, "CONN #" PRIu32 " %s ...", id, ...);
```

`PRIu32` expands to the conversion letters only. With no preceding `%`, the first real conversion in the resulting format was `%s`. Varargs therefore supplied `id == 1` as that `%s` pointer. `vfprintf()` called `strnlen((char *)0x1)`, producing `LoadProhibited` with `EXCVADDR=0x00000001`.

The corrected expression is `"CONN #%" PRIu32 " %s ..."`.

Five nearby `CONN #` and one `AUTO scan #` expressions had the same malformed PRI concatenation and were corrected.

## FORMAT AUDIT

**SOURCE PROOF:** `scripts/check_format_strings.py` inspected `src`, `components`, and `tests`: 332 printf-style calls and 31 remaining PRI uses. It validates every source occurrence in which a string literal is concatenated with a PRI macro and requires the literal to end in `%`.

**CONFIRMED defects:**

1. Six missing `%` characters before `PRIu32` in `wifi_worker.c`.
2. Two `size_t` heap values in `wifi_analyzer.c` were passed to `PRIu32` (`%lu` for this Xtensa ABI). Both now use `%zu`.

No other confirmed format mismatch remained after strict production compilation.

## WHY THE COMPILER MISSED IT

**BUILD PROOF:** the old real `wifi_worker.c` compile command already contained `-Wall -Werror -Wextra`. Adding `-Wformat=2` alone also did not reject the malformed call in a negative production compile probe.

**SOURCE PROOF:** this build uses `CONFIG_LOG_VERSION_1=y`. ESP-IDF 6.2 declares the active `esp_log()` in `components/log/include/esp_log.h` as:

```c
void esp_log(esp_log_config_t config, const char *tag,
             const char *format, ...);
```

That declaration has no GCC `format(printf, 3, 4)` attribute. `esp_log_write()` has an attribute, but Log V1 `ESP_LOGI` expands through `esp_log()`, so GCC could not associate the format string with its varargs. Host state-machine tests do not compile `wifi_worker.c` or expand its ESP_LOG calls.

## PREVENTION

`src/iotse_format_check.h` redeclares `esp_log()` with `__attribute__((format(printf, 3, 4)))` for IoTSE production translation units. The IoTSE component now compiles with:

- `-Wformat=2`
- `-Werror=format`
- `-Werror=format-security`

A mandatory `iotse_format_audit` ALL target scans real repository sources on every build. A negative regression temporarily restored the exact malformed expression in the real `wifi_worker.c`; the audit failed at line 68. After restoring the corrected source, the production object compiled successfully.

## CONNECTION-PATH CRASH AUDIT

**CONFIRMED:** the six integer-as-pointer-capable PRI logging defects and the two analyzer `size_t` type mismatches described above.

**SOURCE PROOF — no defect found:** UI commands are copied by value into a queue whose item size is `sizeof(wifi_worker_cmd_t)`. Radio events are copied by value into a queue sized as `sizeof(radio_event_t)`. Callback context is static/null. `start_attempt()` consumes its command synchronously and copies a manual command into static worker ownership. SSID/password command buffers originate from zero-initialized structs and bounded lengths; decoded Known Networks entries start from a zeroed database and validate serialized lengths. BSSID copies are fixed at six bytes. No heap-owned connection pointer crosses a callback or queue boundary.

**POTENTIAL — NOT YET PROVEN:** `finish_scan()` places `wifi_scan_data_t` plus the visible-candidate array on the 6144-byte worker stack. Current builds pass, but only the on-device worker stack high-water mark proves margin under scan/connect/preemption stress. No code change was made without hardware evidence.

## TEST RESULTS

**PASS:** repository format audit: 332 calls, 31 PRI uses.

**PASS:** negative regression: malformed real `wifi_worker.c` rejected; corrected production object accepted.

**PASS:**

- `wifi_connection_manager_unit` — 20 scenarios;
- `wifi_known_networks_unit`;
- `wifi_connection_post_audit_unit` — 20 targeted cases;
- `wifi_parser_unit`;
- `wifi_ap_tracker_unit`;
- `wifi_sta_tracker_unit`;
- `wifi_sta_raw_integration`;
- `wifi_sta_stress` — 50,000 observations.

All host tests used `-Wall -Wextra -Werror`. `git diff --check` passed with only existing LF/CRLF notices.

## BUILD

**BUILD PROOF — PASS:** complete ESP-IDF 6.2 ESP32-S3 build.

- `IoTSE.bin`: 1,104,144 bytes (`0x10d910`)
- SHA-256: `F82A43B2F7F4F8CE112AE787C8E3E70B8A3D30ED0BDF5C0E316A6100E5C73E3D`
- smallest app partition: `0x400000`
- free: `0x2f26f0` bytes (74%)

## HARDWARE RETEST PLAN

**HARDWARE PROOF: pending. This remediation was not flashed automatically.**

1. Boot and watch for panic/reset.
2. Open Wi-Fi and scan.
3. Manual Connect; verify logs show `CONN #1 MANUAL ...` safely.
4. Observe `ASSOCIATING -> WAITING_IP -> CONNECTED`.
5. Disconnect and reconnect.
6. Start and stop Air Monitor.
7. Reconnect again.
8. Watch worker stack HWM and confirm no `LoadProhibited`, watchdog, queue drop, or unexpected reboot.
