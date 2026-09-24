# OpenThread Border Router Example on a Single ESP32-C6 (Native Radio)

## Overview

This variant runs the Border Router — including the REST API / web GUI from
`esp_ot_br_server` — on a single ESP32-C6, using its native 802.15.4 radio
instead of an external RCP chip. No second SoC, no UART wiring between two
boards.

**This is not Espressif's recommended configuration.** [README_standalone_RCP.md](README_standalone_RCP.md)
explicitly notes that the C-series chips have only one RF path, so Wi-Fi and
Thread can't receive simultaneously, and recommends a two-chip setup instead.
The plain [`ot_br` example in esp-idf](https://github.com/espressif/esp-idf/tree/master/examples/openthread/ot_br)
does support and CI-test this native-radio mode on the C6
(`sdkconfig.ci.native_radio`) — it's just that example has no REST API. This
variant exists to get the REST API onto that same single-chip setup, for
people who want one board and don't need the raw throughput of a dedicated
two-chip solution. Expect a real but usually acceptable coexistence
performance hit, not a broken device.

## Changes from upstream

Three small, targeted changes, all guarded by `CONFIG_OPENTHREAD_RADIO_NATIVE`
so every other target/configuration in this repo is untouched:

- `main/esp_ot_config.h` — added a `RADIO_MODE_NATIVE` branch to
  `ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG()`, lifted from esp-idf's own `ot_br`
  example.
- `main/esp_ot_br.c` — native radio needs one more eventfd for the radio
  driver (the same accounting `ot_br` does); added it to the `max_eventfd`
  calculation. Without this, the eventfd pool that gets registered by
  `esp_vfs_eventfd_register()` will be one short.
- `sdkconfig.defaults.esp32c6` (new) — sets `CONFIG_OPENTHREAD_RADIO_NATIVE=y`,
  `CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y`, `CONFIG_AUTO_UPDATE_RCP=n` (nothing to
  auto-update without an RCP), and `CONFIG_OPENTHREAD_BR_START_WEB=y`.

## How to use

```
idf.py set-target esp32c6
idf.py menuconfig   # set your Wi-Fi SSID/password under Example Connection Configuration
idf.py build flash monitor
```

`OPENTHREAD_BR_AUTO_START` is left at its default (off), so Wi-Fi connects at
boot but the Thread network is only formed or joined once you call the REST
API — there's no Home Assistant here to auto-provision it. See the API spec
at `components/esp_ot_br_server/src/openapi.yaml`; the short version:

```bash
curl -X PUT "http://<device-ip>/node/dataset/active" \
  -H "Content-Type: application/json" -d '{"NetworkName": "MyThreadNet"}'
curl -X PUT "http://<device-ip>/node/state" \
  -H "Content-Type: application/json" -d '"enable"'
```

Any dataset fields you omit are randomized, matching how the firmware's own
`OPENTHREAD_BR_AUTO_START` path creates a network when no dataset exists.

## Hardware Required

Any ESP32-C6 dev board with **8MB flash**. Avoid 4MB variants — this example's
partition table (`ota_0`/`ota_1`/`web_storage`/`rcp_fw`) doesn't fit
comfortably otherwise, even though `rcp_fw` goes unused here.
