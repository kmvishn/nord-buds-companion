# Nord Buds companion for Linux

A native Linux (GNOME) companion app for the **OnePlus Nord Buds 2r**: the battery, sound and touch
settings you normally get from HeyMelody or a OnePlus/OPPO phone, on your Ubuntu desktop.

- **Battery** for the left bud, right bud and case, with charging state
- **Sound profile**: Balanced, Bass, Bold
- **Game mode** (low latency) on/off
- **Touch controls** for each bud: single, double and triple tap, touch and hold
- **Find my earbuds**: rings both buds (stops by itself after 60 s)
- **Connect / disconnect** Bluetooth audio
- **Firmware** version of each bud and the case
- **Top bar**: icon with battery text and quick controls, with a choice of position and label
- **Low-battery notification** at 15 % or less
- **Command line**: everything above from a terminal
- Starts at login and runs quietly in the background

Written in C++20 with GTK 4. No extra Bluetooth libraries are needed.

---

## Contents

1. [Requirements](#requirements)
2. [Build and install](#build-and-install)
3. [Using it](#using-it)
4. [Top bar](#top-bar)
5. [Command line](#command-line)
6. [Settings and files](#settings-and-files)
7. [Uninstall](#uninstall)
8. [How it works](#how-it-works)
9. [Troubleshooting](#troubleshooting)
10. [Limitations](#limitations)
11. [Project layout](#project-layout)
12. [Credits](#credits)

---

## Requirements

- Ubuntu 24.04 or another Linux with GNOME 45–50 (tested on GNOME 46, Wayland)
- BlueZ (the normal Linux Bluetooth stack)
- Build tools: `cmake`, `g++` (C++20), `pkg-config`, `libgtk-4-dev`

```bash
sudo apt install cmake g++ pkg-config libgtk-4-dev
```

The earbuds must already be **paired** in *Settings → Bluetooth*.

## Build and install

```bash
git clone git@github.com:kmvishn/nord-buds-companion.git
cd nord-buds-companion/linux
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=$HOME/.local
cmake --build build -j
cmake --install build
```

This installs, all inside your home folder:

| File | Purpose |
|---|---|
| `~/.local/bin/nordbuds` | the app and command-line tool |
| `~/.local/share/applications/com.nordbuds.Companion.desktop` | "Nord Buds" in the app menu |
| `~/.config/autostart/nordbuds-autostart.desktop` | starts the background service at login |
| `~/.local/share/gnome-shell/extensions/nordbuds@nordbuds.local/` | GNOME top-bar extension |
| `~/.local/share/nordbuds/icons/nordbuds-symbolic.svg` | top-bar icon (AppIndicator fallback) |
| `~/.local/share/icons/hicolor/scalable/apps/nordbuds-symbolic.svg` | same icon for the window (GTK only recolours symbolic icons inside a theme) |

Build options (add to the first `cmake` line):

| Option | Default | Effect |
|---|---|---|
| `-DNORDBUDS_AUTOSTART=OFF` | ON | do not start at login |
| `-DNORDBUDS_GNOME_EXTENSION=OFF` | ON | do not install the GNOME extension |

### Turn on the top-bar extension (once)

```bash
gnome-extensions enable nordbuds@nordbuds.local
```

On Wayland, GNOME only loads a newly installed extension at login, so **log out and back in once**.
Until then, the app shows its fallback icon through the Ubuntu AppIndicator extension (see [Top bar](#top-bar)).

### Start it now (without logging out)

```bash
nordbuds --background   # top-bar only
nordbuds                # opens the window too
```

After an update, restart it with `nordbuds quit && nordbuds --background`.

## Using it

- **App menu → Nord Buds**, or run `nordbuds`, to open the window.
- **Top bar**: click the earbuds icon for battery and quick controls.
- **Close the window** to hide it. The service keeps running in the top bar. Use **Quit** in the top-bar
  menu, or `nordbuds quit`, to stop it.

The window follows your light or dark theme and has:

| Section | What it does |
|---|---|
| Status card | name, live status dot (green connected, amber connecting), Connect / Disconnect |
| Battery | animated ring per left bud, right bud and case. Green ≥ 50 %, amber ≥ 20 %, red below (marked "Low"), blue while charging |
| Sound | three sound-profile tiles (Balanced / Bass / Bold) with a sketch of each curve, game mode switch |
| Touch controls | Left / Right earbud switcher, one dropdown per gesture. Touch and hold is fixed to Switch device by the firmware |
| Find my earbuds | round ring button. It pulses red while ringing; press again to stop |
| ☰ menu (header) | top-bar position, order and label style; firmware and Bluetooth address |

While the buds are not connected, the window shows only the status card, with a hint or a spinner. It
never shows the last battery or settings it knew.

Every change is sent to the buds, then **read back** from them. The window only shows what the buds
actually report.

### Touch control options (Nord Buds 2r)

| Gesture | Choices |
|---|---|
| Single tap | None, Play / pause |
| Double tap | None, Play / pause, Previous track, Next track, Voice assistant, Game mode |
| Triple tap | None, Previous track, Next track, Voice assistant, Game mode |
| Touch and hold | Switch device (the buds refuse "None" here) |

Factory setting: single tap play/pause, double tap next, triple tap previous, hold switches device.

## Top bar

There are two ways the top-bar item can appear. The app picks automatically:

1. **Nord Buds GNOME extension** (preferred). A native panel button with:
   - the earbuds icon and your chosen battery label
   - a menu with battery rings for left, right and case, sound-profile buttons, game mode, ring earbuds,
     connect/disconnect, open window
   - your chosen **position** (left, center or right area of the top bar) and **order**
2. **AppIndicator fallback**. Used when the extension isn't running (for example, before your first
   re-login). It shows the same icon, label and menu through Ubuntu's AppIndicator extension. Its
   position is shared with all tray icons and is set in that extension's own settings.

When the extension is running, the fallback icon hides itself, so you never see two.

### Options

| Setting | Values | Default |
|---|---|---|
| Position | `left`, `center`, `right` | `right` |
| Order | `0`–`20` (0 = first item in that area) | `0` |
| Label | `icon` (icon only), `compact` (lowest bud, e.g. `90%`), `buds` (`L 90%  R 100%`), `all` (`L 90%  R 100%  C 60%`) | `compact` |

Change them in the window (**☰** menu in the header) or from a terminal:

```bash
nordbuds panel                    # show current settings
nordbuds panel position center
nordbuds panel order 2
nordbuds panel label all
```

Changes apply immediately. Position and order need the GNOME extension.

## Command line

```
nordbuds [--device AA:BB:CC:DD:EE:FF] [command]
```

| Command | Description |
|---|---|
| *(none)* | open the window and keep the top-bar item running |
| `--background`, `-b` | top bar only (what login uses) |
| `status` | everything: link, battery, sound, game mode, firmware, touch controls |
| `battery` | `left 90%   right 100%   case 60%` |
| `eq` | show the sound profile |
| `eq balanced` \| `bold` \| `bass` | set the sound profile |
| `game` / `game on` / `game off` | show or set game mode |
| `find` / `find off` | ring the earbuds / stop |
| `touch` | show touch controls |
| `touch <left\|right\|both> <single\|double\|triple\|hold> <action>` | set a touch control |
| `connect` / `disconnect` | Bluetooth audio connect / disconnect |
| `panel ...` | top-bar settings (see above) |
| `show` | open the window |
| `quit` | stop the background service |
| `help` | usage |

Touch actions: `none`, `play-pause`, `previous`, `next`, `voice-assistant`, `game-mode`, `switch-device`.

Examples:

```bash
nordbuds status
nordbuds eq bass
nordbuds game on
nordbuds touch both double voice-assistant
nordbuds touch left triple next
```

Exit codes: `0` success, `1` the earbuds were not reachable or refused the change, `2` bad arguments.

If the background service is running, commands go through it and answer instantly. If it isn't, the
command connects by itself (about 2 s), does the job, and exits.

`--device` (or the `NORDBUDS_DEVICE` environment variable) picks a specific Bluetooth address. Without it,
the app uses the paired OnePlus/OPPO earbuds, preferring connected ones.

## Settings and files

| Path | Contents |
|---|---|
| `~/.config/nordbuds/settings.ini` | top-bar position, order and label |

```ini
[panel]
position=right
order=0
label=compact
```

Earbud settings (sound profile, game mode, touch controls) are stored **on the buds themselves**, so
they follow the buds to your phone and back.

## Uninstall

```bash
nordbuds quit
gnome-extensions disable nordbuds@nordbuds.local
rm -f  ~/.local/bin/nordbuds \
       ~/.local/share/applications/com.nordbuds.Companion.desktop \
       ~/.config/autostart/nordbuds-autostart.desktop
rm -f  ~/.local/share/icons/hicolor/scalable/apps/nordbuds-symbolic.svg
rm -rf ~/.local/share/gnome-shell/extensions/nordbuds@nordbuds.local \
       ~/.local/share/nordbuds ~/.config/nordbuds
```

## How it works

```
 ┌──────────────┐  RFCOMM (classic BT)  ┌──────────────────────┐   session D-Bus    ┌────────────────────┐
 │ Nord Buds 2r │◄─────────────────────►│ nordbuds (service)   │◄──────────────────►│ GNOME extension    │
 └──────────────┘  "oppointeraction"    │  ├ window (GTK 4)    │  StatusNotifierItem│ or AppIndicator    │
        ▲          channel via SDP      │  ├ CLI (GApplication)│◄──────────────────►│ fallback           │
        │                               │  └ notifications     │                    └────────────────────┘
        └──── BlueZ (system D-Bus): finds the paired buds, connect/disconnect, link state
```

- **Transport.** The buds use **classic Bluetooth RFCOMM**, not Bluetooth LE. The app looks up the
  vendor service (`00001107-d102-11e1-9b23-00025b00a5a5`, named `oppointeraction`) over SDP. It's on
  channel 12 on these buds. The app then opens a socket to it directly through the kernel, so no
  libbluetooth is needed.
- **Framing.** `AA <len> 00 00 <cmd u16 LE> <seq> <len u16 LE> <payload>`. A reply has bit 15 set on
  the command (`0x0106` → `0x8106`). Unprompted updates arrive as command `0x0204` with sequence `FF`.
- **Startup.** Reads the supported-command bitmap (`0x0100`) and subscribes to battery and game-mode
  updates (`0x0205`). Then it reads firmware, battery, sound profile, game mode and touch controls, one
  message every 200 ms. It also answers the buds' own requests (for example the time request
  `0x0500`).
- **Updates.** The buds push battery changes. The app also re-reads battery every 60 s. Every write is
  followed by a read-back.
- **Disconnects.** When the link goes down (BlueZ reports it, the socket closes, or the buds stop
  answering two battery polls in a row), all device state is cleared. The window, top-bar label, menu
  and D-Bus state then report "unknown" instead of the last values, and any low-battery notification
  is withdrawn.
- **The service** is a single `GApplication` (`com.nordbuds.Companion`). Command-line runs talk to the
  running instance, and the GNOME extension uses its D-Bus API:
  `com.nordbuds.Companion.Buds` at `/com/nordbuds/Companion/Buds`, with `GetState`, `SetEq`,
  `SetGameMode`, `Find`, `Connect`, `Disconnect`, `OpenWindow` and the `StateChanged` signal.

| Feature | Read | Write |
|---|---|---|
| Battery | `0x0106` → `00 <n> (<part> <level\|0x80 charging>)…`; part 1 L, 2 R, 3 case | – |
| Firmware | `0x0105` → `"1,2,113,2,2,113,3,2,101"` | – |
| Sound profile | `0x010F` → `00 <id>` | `0x0406 <id>`; 0 Balanced, 1 Bold, 2 Bass |
| Game mode | `0x010D 02 05 06` → `… 06 <0/1>` | `0x0403 06 <0/1>` |
| Touch controls | `0x0108 02 03 01` → `00 <n> (<side> 01 <gesture> <action>)…` | `0x0401 <n> <changed entries>` |
| Find | – | `0x0400 <1/0>` |

## Troubleshooting

| Symptom | Fix |
|---|---|
| "No paired OnePlus Nord Buds found" | Pair them in *Settings → Bluetooth* first. |
| "Not connected" | Take the buds out of the case, or click **Connect** / run `nordbuds connect`. |
| Case battery shows "–" | The case only reports while its lid is open. |
| Stuck on "Connecting…" | Close HeyMelody or other apps that may be holding the control channel. Toggle Bluetooth off and on. |
| Top-bar item is the AppIndicator one, position settings greyed out | Run `gnome-extensions enable nordbuds@nordbuds.local`, then log out and back in. |
| Extension not showing after login | `gnome-extensions info nordbuds@nordbuds.local` shows its state. Errors are in `journalctl --user -b -g "Nord Buds"`. |
| Two icons in the top bar | Restart the service: `nordbuds quit && nordbuds --background`. |

## Limitations

- Built and tested for the **Nord Buds 2r** (product `062C14`, firmware `113.113.101`). Other
  OnePlus/OPPO/realme buds use the same protocol, but their presets and gesture codes differ, so they
  may only partly work.
- The Nord Buds 2r has **no noise cancellation and no wear detection**, so neither is in the app.
- **Custom EQ** (your own band sliders) is supported by the buds but not built yet.
- Tested on real hardware: status, battery, all sound profiles, game mode, touch controls on both buds,
  top-bar menu actions, `find off`. Not exercised on hardware: `find on` (loud tone) and
  connect/disconnect (cuts audio). Both use standard commands.

## Project layout

```
nord-buds-companion/linux/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── main.cpp         app, background service, command line
│   ├── buds.{h,cpp}     device link: BlueZ discovery, RFCOMM, startup sequence, state
│   ├── protocol.{h,cpp} frame encode/decode, battery/firmware/gesture parsing
│   ├── model.{h,cpp}    Nord Buds 2r presets and touch options
│   ├── bt.{h,cpp}       SDP lookup and RFCOMM socket (kernel ABI)
│   ├── window.{h,cpp}   GTK 4 window
│   ├── indicator.{h,cpp} AppIndicator / StatusNotifierItem fallback icon and menu
│   ├── api.{h,cpp}      session D-Bus API for the GNOME extension
│   └── settings.{h,cpp} top-bar settings (settings.ini)
├── gnome-extension/     GNOME Shell extension (extension.js, stylesheet.css, metadata.json)
└── data/                .desktop templates and the earbuds icon
```

The Windows port lives next to this folder in `../windows/`.

## Credits

The protocol knowledge comes from these open-source projects:

- [QuickBuds](https://github.com/spizganed/QuickBuds): protocol documentation and per-model data
  (EQ ids and gesture options for the Nord Buds 2r)
- [Gadgetbridge](https://github.com/Freeyourgadget/Gadgetbridge): OPPO headphones protocol
  (battery, firmware, touch config)

Not affiliated with OnePlus or OPPO.
