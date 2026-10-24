# Nord Buds companion

Desktop companion apps for the **OnePlus Nord Buds 2r**: battery (buds and case), sound profile,
game mode, touch controls, find my earbuds and connect/disconnect.

| Folder | Platform | Details |
|---|---|---|
| [`linux/`](linux/README.md) | Ubuntu / GNOME (GTK 4, top-bar extension) | in daily use, tested with the earbuds |
| [`windows/`](windows/README.md) | Windows 10 / 11 (Win32 + Direct2D, tray icon) | builds; not yet tested on a Windows PC |

Both are C++20 and share the same protocol code (`src/protocol.*`, `src/model.*`).
