# Nord Buds companion

Desktop companion apps for the **OnePlus Nord Buds 2r**: battery (buds and case), sound profile,
game mode, touch controls, find my earbuds and connect/disconnect.

| Folder | Platform | Details |
|---|---|---|
| [`linux/`](linux/README.md) | Ubuntu / GNOME (GTK 4, top-bar extension) | in daily use, tested with earbuds (`.deb` available) |
| [`windows/`](windows/README.md) | Windows 10 / 11 (Win32 + Direct2D, tray icon) | tested on Windows 10/11 (`NordBuds-Setup.exe` available) |

Both are C++20 and share the same protocol code (`src/protocol.*`, `src/model.*`).

## Downloads

Download the latest installer or package from the [Releases](https://github.com/kmvishn/nord-buds-companion/releases) page:
- **Windows**: `NordBuds-Setup.exe` (or portable `.zip`)
- **Linux**: `nordbuds_1.0.0_amd64.deb` (or portable `.tar.gz`)

