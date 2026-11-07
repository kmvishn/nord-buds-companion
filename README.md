# Nord Buds Companion

[![Latest Release](https://img.shields.io/github/v/release/kmvishn/nord-buds-companion?color=blue&logo=github)](https://github.com/kmvishn/nord-buds-companion/releases)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg?logo=c%2B%2B)](https://en.wikipedia.org/wiki/C%2B%2B20)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-lightgrey)](https://github.com/kmvishn/nord-buds-companion)

A native, zero-runtime desktop companion suite and CLI for the **OnePlus Nord Buds 2r** (model `062C14`). Brings smartphone HeyMelody features directly to your desktop.

---

## Features

- 🔋 **Independent Battery Levels**: Live percentage for Left bud, Right bud, and charging Case, with charging state indicators.
- 🎵 **Sound Profiles (EQ)**: Balanced, Bass, and Bold presets with frequency response curve visualizations.
- ⚡ **Low-Latency Game Mode**: One-click toggle for reduced audio delay.
- 👆 **Touch Controls Customization**: Remap Single, Double, Triple tap, and Touch & Hold gestures per earbud.
- 🔔 **Find My Earbuds**: Acoustic ring locator (stops automatically after 60 seconds).
- 📶 **Bluetooth Connect / Disconnect**: One-click audio device connection management.
- 🖥️ **Desktop Integration**:
  - **Windows**: Modern Direct2D/DirectWrite UI, dynamic tray icon badge showing battery level and color-coded status bar, and PerMonitorV2 DPI awareness.
  - **Linux**: GTK 4 interface, native GNOME Shell top-bar extension, and AppIndicator fallback.
- 💻 **Full CLI**: Automate and inspect everything from your terminal with `nordbuds`.

---

## Downloads

Download the latest installer or package from the **[Releases](https://github.com/kmvishn/nord-buds-companion/releases/latest)** page:

| Platform | Recommended Package | Portable Version |
|---|---|---|
| **Windows 10 / 11** | [**`NordBuds-Setup.exe`**](https://github.com/kmvishn/nord-buds-companion/releases/latest/download/NordBuds-Setup.exe) (Setup wizard, Start menu, PATH) | [**`nordbuds-windows-x64.zip`**](https://github.com/kmvishn/nord-buds-companion/releases/latest/download/nordbuds-windows-x64.zip) |
| **Linux (Ubuntu / Debian / GNOME)** | [**`nordbuds_1.0.0_amd64.deb`**](https://github.com/kmvishn/nord-buds-companion/releases/latest/download/nordbuds_1.0.0_amd64.deb) (Debian package) | [**`nordbuds-linux-x64.tar.gz`**](https://github.com/kmvishn/nord-buds-companion/releases/latest/download/nordbuds-linux-x64.tar.gz) |

---

## 🛠️ Fork & Make It Work for Your Earbuds!

Most wireless earbuds from **OnePlus**, **OPPO**, and **Realme** (such as OnePlus Buds Pro, Buds Pro 2 / 3, Nord Buds / Buds 2, OnePlus Buds Z2, OPPO Enco X / Air / Free, and Realme Buds Air) communicate using the **exact same underlying protocol**:

- **RFCOMM Control Channel**: Service UUID `00001107-d102-11e1-9b23-00025b00a5a5` (`oppointeraction`).
- **Framing & Transport**: Fully implemented in [`src/protocol.cpp`](windows/src/protocol.cpp) (`AA <len> 00 00 <cmd> <seq> <paylen> <payload>`).
- **Telemetry**: Battery levels, charging state, firmware strings, time requests, and paging tones follow the same framing across the ecosystem.

### How to adapt this for your model:

1. **Fork the repository** on GitHub.
2. **Device Discovery**:
   - Update the device name matching in `src/buds.cpp` (or specify your earbuds' MAC address via `--device AA:BB:CC:DD:EE:FF` / `NORDBUDS_DEVICE` environment variable).
3. **Model Presets & Gestures**:
   - Update [`src/model.cpp`](windows/src/model.cpp) with your model's EQ preset IDs, custom equalizer bands, or touch gesture options (e.g., adding ANC / Transparency toggles if supported).
   - Detailed model codes and command tables can be referenced from [QuickBuds](https://github.com/spizganed/QuickBuds) and [Gadgetbridge](https://github.com/Freeyourgadget/Gadgetbridge).
4. **Build & Package**:
   - **Windows**: `cmake --build build --target installer` generates your custom `NordBuds-Setup.exe`.
   - **Linux**: `cpack --config build/CPackConfig.cmake` generates your custom `.deb`.

Pull requests expanding support to additional OnePlus / OPPO / Realme models are warmly welcomed!

---

## Documentation

| Guide | Description |
|---|---|
| 🐧 [**Linux Companion Guide**](linux/README.md) | Ubuntu / GNOME Shell extension, GTK 4, D-Bus service, autostart |
| 🪟 [**Windows Companion Guide**](windows/README.md) | Win32 + Direct2D tray service, Inno Setup installer, CLI IPC |

Both ports share the core protocol framing (`src/protocol.*`) and model definition logic (`src/model.*`).
