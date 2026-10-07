# IoTSE reliability audit — 2026-09-29

This document records the current backend audit, the fixes applied, and the
remaining work. A successful build or boot is not treated as proof that every
runtime path is failure-free; hardware stress tests remain mandatory.

## Fixed in this pass

### Wi-Fi worker

- Terminal scan/connect events can no longer be silently lost when the radio
  event queue is full. A bounded atomic fallback mailbox lets the single worker
  consume the latest terminal transition.
- Scan and connect operations now have 20 second deadlines. Disconnect and AP
  switch operations have 5 second deadlines.
- A timeout clears the active request, publishes a deterministic error for a
  user operation, restarts the radio, and schedules a later retry for automatic
  connection. The worker can no longer remain permanently busy after a missing
  SDK callback.

### Capture store

- Initialization is transactional. Allocation, mutex, queue, or task creation
  failure releases everything allocated by that attempt.
- Save state, result state, and packet count checks are protected by the module
  mutex. Queue submission failure also rolls back the saving flag safely.

### File Manager and internal storage

- FAT mount failure never formats internal storage automatically. Recovery and
  formatting must be explicit user actions.
- Names that do not fit the fixed 64-byte command field are rejected instead of
  being truncated into a different valid path.
- Delete re-checks the object type with `stat()` immediately before operating.
- Volume registration validates its descriptor, path, duplicates, and table
  capacity and returns an error which `app_main()` handles.
- FatFs locking is enabled for eight simultaneously open file objects.

### SD card foundation

`storage_sd` provides a transport-independent lifecycle for `/sdcard`:

- SDMMC and SDSPI mount entry points accept board-owned host/pin structures;
- mounting never formats a card;
- state, generation, capacity, last error, and active IO count are observable;
- IO leases prevent eject while a filesystem operation owns the card;
- failed unmount remains in `MOUNTED`, allowing a safe retry and preventing a
  second mount over a live VFS instance.

The SD volume is deliberately not registered in File Manager yet. Registration
before the actual board transport and pins are known would expose a volume that
cannot be mounted or safely removed.

## Hardware evidence

- Full ESP-IDF build passed.
- `IoTSE.bin`: 1,045,648 bytes; SHA-256
  `B6CA9DD74F269333CDF18F680C23AF99B930B229693569558AF767FFFA643EB0`.
- JTAG programming and flash verification passed.
- Boot smoke test passed: 8 MB PSRAM test, internal FAT mount, FM worker, Wi-Fi
  worker/scanner, shared SPI, display DMA, UI, and input bridge all initialized.
- At approximately -82 dBm the AP rejected two authentication attempts with
  reason 2; automatic retry then associated successfully. No panic, watchdog
  reset, or stack overflow appeared in the captured log.

### SD hardware attempt

The shared-bus image was flashed and verified after wiring the socket as
SCK=18, MOSI=17, MISO=8, CS=3. The card responds to the initial SPI probe, but
SCR transfer fails with `ESP_ERR_INVALID_CRC`. Repeating at a conservative
10 MHz host limit produced the same result. Firmware continues safely without
mounting or formatting the card, and the display initializes normally after
the failed attempt. Check card power/ground, MISO continuity, CS continuity,
wire length, pull-ups, and whether any display SDO pin is driving shared MISO.

## Required before mounting the physical SD card

Board wiring is now fixed for the shared SPI2 bus:

- SCK: GPIO18;
- MOSI: GPIO17;
- MISO: GPIO8;
- SD CS: GPIO3;
- display CS: GPIO7.

`storage_sd_mount_board()` uses this configuration and the existing bus; it
does not initialize a second SPI bus. SD mount, unmount, and leased IO take the
same logical mutex as display frame transfers.

Still record the remaining hardware properties before automatic mounting:

1. card-detect GPIO and active level, if present;
2. power-enable GPIO and stabilization delay, if present;
3. maximum supported bus frequency and card supply voltage.

Then add one storage worker which owns mount/eject, register a static SD volume
with FM, and require every FM/export/capture operation on `/sdcard` to hold an
IO lease for its full VFS operation.

## Remaining risks, in priority order

1. Display DMA timeout currently leaves the shared SPI lock held. Before SDSPI
   can share that bus, introduce a bus health state and a controlled display/bus
   recovery path; otherwise one display timeout can block the card forever.
2. FM publishes pointers into a double-buffer cache. A consumer can retain a
   pointer while the worker rotates and overwrites that bank. Replace this with
   copied snapshots or generation-checked acquire/release ownership.
3. FM OPEN/SAVE APIs accept absolute paths. Constrain every public operation to
   a registered mount root before SD or external callers use them.
4. The one-slot FM event queue has no request ID. Add request IDs/client routing
   before a second UI or background storage client is introduced.
5. Promiscuous receive shutdown needs a callback/task lifetime handshake before
   deinitialization can be added safely.
6. Add high-watermark, minimum-heap, queue-overflow, Wi-Fi recovery, and storage
   error counters to a single diagnostics snapshot.
7. Saved Wi-Fi credentials are plain NVS data. Production hardware should use
   NVS encryption/flash encryption and secure boot according to its threat
   model.
8. Scanner results should bind target IP, BSSID, SSID, and connection generation
   so stale results cannot be presented after an AP change.

## Stress-test gate before the next module

- 50–100 Quick scans against the same laboratory host; compare open ports and
  banners with Nmap and watch socket count, minimum internal heap, PSRAM, and
  scanner task high-watermark.
- Cancel scans at discovery, TCP connect, and banner phases.
- Repeatedly disconnect/reconnect and switch AP during idle and immediately
  after a scan.
- Fill the FM command queue, exercise long and invalid names, and repeat
  create/rename/delete/open/save while capture export runs.
- After hardware integration, mount/eject SD repeatedly, remove it during idle,
  simulate unmount failure, and test full/read-only/corrupt cards without any
  automatic format.
