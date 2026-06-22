# Prebuilt relay firmware

J-Link / SWD-flashable images for the MouthPad relay.

| File | Board | Variant | Version | Source commit |
|------|-------|---------|---------|---------------|
| `mouthpad-relay-dotto-v0.1.6.hex` | `vox_dotto/nrf52840` | Production Dotto (KTD2026 @ 0x32) | 0.1.6 | `79b9870` |
| `mouthpad-relay-dotto-v0.1.5.hex` | `vox_dotto/nrf52840` | Production Dotto (KTD2026 @ 0x32) | 0.1.5 | `fc21eff` |
| `mouthpad-relay-dotto-v0.1.4.hex` | `vox_dotto/nrf52840` | Production Dotto (KTD2026 @ 0x32) | 0.1.4 | `e51f340` |

Changes in 0.1.6 (since 0.1.5): the relay↔MouthPad link now defaults to 2M and
only switches to Coded (S2/S8) when the companion explicitly requests it.

Changes in 0.1.5 (since 0.1.4): runtime relay↔MouthPad link-PHY selection
(1M/2M/Coded S2/S8), the active link PHY reported alongside RSSI, auto-unpair on
encryption failure, and clear-bonds now preserves the host↔relay bond.

These are **flat images** (SoftDevice Controller linked into the app, no separate
bootloader) that own flash from 0x0 — they replace whatever is on the chip.

## Flash (J-Link / SWD)

```
nrfjprog -f nrf52 --program firmware/mouthpad-relay-dotto-v0.1.5.hex --chiperase --verify -r
```

`--chiperase` wipes existing bonds; use `--sectorerase` instead to preserve them
(settings_storage lives at 0xFE000).

## Rebuild

Run from the NCS workspace (`C:\ncs`), with the app's `boards/` as the board root:

```
nrfutil toolchain-manager launch --ncs-version v3.3.0 -- \
  west build -b vox_dotto/nrf52840 <repo>/ncs/app -d <repo>/ncs/app/build_dotto \
  --pristine=always -- -DBOARD_ROOT=<repo>/ncs/app
```

The output is `ncs/app/build_dotto/merged.hex`.

### Dev-kit variant (KTD2026 eval board @ 0x30)

Add the dev-kit overlay (quote the flag in PowerShell so `.conf` isn't split):

```
... -- -DBOARD_ROOT=<repo>/ncs/app "-DEXTRA_CONF_FILE=<repo>/ncs/app/dev_kit.conf"
```
