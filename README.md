# Edge-o-Matic 3000

Firmware for the Edge-o-Matic 3000 automated edging device.

## About this fork

Upstream (`MausTec/edge-o-matic-3000`) no longer accepts contributions, and its
`mt-actions` plugin runtime submodule was deleted, which left the firmware unbuildable.
This fork keeps it buildable and self-hosted:

* `lib/mt-actions` -> [Xboarder56/mt-actions](https://github.com/Xboarder56/mt-actions):
  a verified reconstruction of mt-actions v1.3.2 (see that repo for how it was validated).
* Maus-Tec-hosted dependencies are mirrored here. The exact commits the release pins are
  protected by an `eom3k-v2.1.0-rc.1` tag in each fork:
  [eom-hal-dist](https://github.com/Xboarder56/eom-hal-dist) (precompiled HAL),
  [ts-code](https://github.com/Xboarder56/ts-code),
  [mt-accessory-common](https://github.com/Xboarder56/mt-accessory-common),
  [tlibmp](https://github.com/Xboarder56/tlibmp).
* Third-party submodules (u8g2, semver.c, wasm-micro-runtime) still point at their upstreams.
* CI builds on every push and weekly (to catch a vanished dependency early). Tags `v*`
  publish a GitHub Release with the app image (`eom3k-firmware-<version>.bin` since 2.1.0-rc.5,
  `firmware.bin` before), `bootloader.bin`, `partitions.bin` and `plugin-api.json`. The app image
  is what USB flashing writes at `0x10000` and what the on-device updater downloads.

### Building

Requirements: git, Python 3.11, about 2 GB of disk for the PlatformIO ESP-IDF 4.4.6 toolchain,
and **a checkout path without spaces** (ESP-IDF 4.4 refuses to build from one, even through a
symlink).

```sh
git clone --recurse-submodules https://github.com/Xboarder56/edge-o-matic-3000.git
cd edge-o-matic-3000
python3.11 -m venv .venv
.venv/bin/pip install platformio
.venv/bin/pio run -e eom3k                 # -> .pio/build/eom3k/firmware.bin
.venv/bin/pio run -e eom3k -t upload       # flash over USB (app only)
.venv/bin/pio device monitor -b 115200     # serial console
```

The firmware version comes from `git describe` (`bin/pio/build_flags.py`). Building exactly
on a tag in a detached checkout gives e.g. `2.1.0-rc.1+HEAD`; commits after a tag add
`+<sha>`. Tag your own releases with a SemVer suffix, e.g. `v2.1.0-rc.1.xb.1`.

Plugins go on the SD card as `/plugins/<name>/plugin.json`; see
[eom-plugins](https://github.com/MausTec/eom-plugins) and [mt-sdk](https://github.com/MausTec/mt-sdk).
This fork adds plugin functions for HTTP(S) requests, Bluetooth reads/notifications/encryption and
byte/crypto helpers: see [doc/PluginHostAPI.md](doc/PluginHostAPI.md).

## Getting Started

Complete setup instructions and documentation are available at [maustec.io/eom](https://maustec.io/eom).

## Web Interface

Connect to your device at [link.maustec.io](http://link.maustec.io) for remote control and monitoring.

## Development

Built with ESP-IDF and PlatformIO. See [doc/](doc/) for technical documentation.

## License

MIT License — see [LICENSE](LICENSE) and [RELICENSING.md](RELICENSING.md).

---

**Edge-o-Matic** is a registered trademark of Maus-Tec LLC.

The Bluetooth® word mark and logos are registered trademarks owned by Bluetooth SIG, Inc.
and any use of such marks by Maus-Tec LLC is under license.
