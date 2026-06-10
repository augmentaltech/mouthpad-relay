# SFP-667 (a): USB firmware updates via the Adafruit nRF52 UF2 bootloader

The relay supports USB firmware updates by running under the **Adafruit nRF52
UF2 bootloader**: the device exposes a USB Mass-Storage drive and you update it
by copying a `.uf2` onto that drive. Entry is triggered by the app (`GPREGRET =
0x57` + reset — see the `dfu` shell command and the `AppToRelayMessage.DfuWrite`
handler in `main.c`), by double-tap reset, or by holding the DFU button at boot.

**The whole flow, validated on the nRF52840 DK:** companion sends a BLE
`DfuWrite` → relay reboots into the UF2 drive → drag `relay.uf2` onto it → app
updates and BLE comes back up → BLE bonds are preserved.

## Flash layout (S140 v6 bootloader)

| Region              | Address           | Notes                                  |
|---------------------|-------------------|----------------------------------------|
| SoftDevice reserved | 0x00000–0x26000   | unused by this Zephyr app (no SD)      |
| Application         | 0x26000–0xEC000   | the relay app (`.uf2` writes here)     |
| settings_storage    | 0xEC000–0xF4000   | BLE bonds (NVS) — untouched by app uf2 |
| UF2 bootloader      | 0xF4000–0x100000  | Adafruit nRF52 bootloader              |

## Building the app `.uf2`

The relay build uses NCS **sysbuild / Partition Manager**, so the layout must be
given to PM via `uf2_pm_static.yml` — NOT just a DTS overlay. (Without it PM puts
`settings_storage` at 0xFE000, *inside* the bootloader region, and `BT_SETTINGS`
init clobbers the bootloader → BLE never advertises. This was the original bug;
modeled on Zephyr's `xiao_ble` board, which ships the same kind of pm_static.)

```sh
# from C:\ncs
nrfutil toolchain-manager launch --ncs-version v3.3.0 -- west build -p always \
  -b vox_dotto/nrf52840 -d <relay>/ncs/build_uf2 <relay>/ncs/app -- \
  -DBOARD_ROOT=<relay>/ncs/app \
  -DEXTRA_DTC_OVERLAY_FILE=uf2.overlay \
  -DEXTRA_CONF_FILE=uf2.conf \
  -DPM_STATIC_YML_FILE=<relay>/ncs/app/uf2_pm_static.yml
# -> build_uf2/app/zephyr/zephyr.uf2  (start address 0x26000)
```

Files: `uf2.overlay` (DTS partitions, mirrors pm_static), `uf2.conf`
(`BUILD_OUTPUT_UF2` + `USE_DT_CODE_PARTITION`), `uf2_pm_static.yml` (the PM
layout — the actual fix). Flat J-Link builds (`build_dotto`, no `-D` flags) are
unaffected.

## Installing the bootloader (one-time, via J-Link)

- **DK bring-up:** Adafruit prebuilt `pca10056_bootloader-0.11.0_s140_6.1.1.hex`
  (`nrfjprog --program <hex> --chiperase --verify --reset`). The DK then mounts
  as `NRF52BOOT`.
- **Production (Dotto):** build an Adafruit_nRF52_Bootloader board variant for
  the Dotto (DFU button P0.31 *active-high*, volume label e.g. `DOTTOBOOT`,
  VID/PID) and flash it. A blank-SoftDevice (`nosd`) build also works and frees
  the 152 KB SD region — but is not required for BLE.

## Updating

1. Trigger DFU (BLE `DfuWrite`, double-tap reset, or DFU button at boot).
2. Copy `zephyr.uf2` onto the mounted drive (find it by volume label; the drive
   letter varies on Windows). The bootloader flashes it and reboots into the app.
