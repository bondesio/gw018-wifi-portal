# GW018 Wi-Fi application diagnostics

The firmware has an optional structured diagnostic listener on TCP port 81.
Port 80 remains the Zigbee UART bridge. The diagnostic service accepts a
single stream client and does not read from or write to the Zigbee UART.

## What the stream contains

The service reports selected startup/network/bridge events, bridge byte and
error counters, event-history loss, FreeRTOS heap readings, and self-reported
health for the TX, RX, bridge, and diagnostics tasks. Health includes fixed
state names, loop heartbeats, successful byte-I/O progress, and sampled stack
space. These observations help narrow down where the application stopped
progressing; they do not prove that the Zigbee radio, electrical flow control,
or the remote client is healthy.

The record format uses fixed field and event names with numeric values. The
firmware does not include Wi-Fi SSIDs or passwords, Zigbee payloads, endpoint
addresses, or arbitrary SDK console strings. It also does not capture ROM,
KM0, or early boot UART output. Records use an SDK tick count, not wall-clock
time; the collector adds UTC timestamps when it receives each record.

## Home Assistant OS collector

Copy `deploy/haos-addon/gw018_wifi_log` into the Home Assistant OS local add-on
directory and install it through the Add-on Store. Set the add-on's `host`
option to the gateway's station/LAN address. Port 81 and a 20-second idle
reconnect timeout are defaults and can be changed in add-on options.

The add-on writes `/config/logs/gateway-debug.log` and rotates five files at
5 MiB each. `/config` is persistent Home Assistant configuration storage, so
logs are not kept on a temporary flashing Raspberry Pi and survive normal
Home Assistant OS upgrades. The collector validates record syntax and field
names, sets restrictive file permissions, and avoids recording exception text
that could expose unrelated data.

The stream is unauthenticated and unencrypted TCP. Use it only on a trusted
local network. Limit access to the gateway's port 81 at the network boundary
if the LAN includes untrusted clients. This listener is for diagnostics; it
has no remote command interface.

## Build and host checks

The normal build instructions in the repository root build both WBRG1 images
and the OTA package from the pinned upstream SDK. From the bundle root, after preparing and applying the overlay to `../gw018-sdk`,
run the host-side fixtures with:

```sh
GW018_SDK_ROOT=../gw018-sdk tests/bridge_health/run.sh
bash tests/gateway_diag/run.sh
GW018_SDK_ROOT=../gw018-sdk tests/lwip_diag/run.sh
python3 tests/collector/test_records.py
```
