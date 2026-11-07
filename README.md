# Nord Buds Companion

[![Latest Release](https://img.shields.io/badge/release-v1.0.0-blue?logo=github)](https://github.com/kmvishn/nord-buds-companion/releases)
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

## Documentation

| Guide | Description |
|---|---|
| 🐧 [**Linux Companion Guide**](linux/README.md) | Ubuntu / GNOME Shell extension, GTK 4, D-Bus service, autostart |
| 🪟 [**Windows Companion Guide**](windows/README.md) | Win32 + Direct2D tray service, Inno Setup installer, CLI IPC |

Both ports share the core protocol framing (`src/protocol.*`) and model definition logic (`src/model.*`).
