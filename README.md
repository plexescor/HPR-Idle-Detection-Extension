# HPR Idle Detection Extension

A native C++ & Lua extension for **HPR** (Human Pattern Recorder) that monitors system idle status and automatically manages tracking lifecycle events.

**Developed by:** Plexescor

## Support Matrix

| Platform / Desktop Environment | Support Status |
| :--- | :--- |
| **Windows** | ✅ Supported |
| **Linux (GNOME)** | ✅ Supported |
| **Linux (KDE Plasma)** | 🚧 In Progress |
| **Linux (Hyprland)** | ✅ Supported |
| **Linux (niri)** | ✅ Supported |
| **Linux (Cinnamon)** | 🚧 In Progress |

## Structure

- `src/` — Native C++ module source code.
- `include/` — C++ header files.
- `external/` — Embedded dependencies (`sol2`, `lua`).
- `idle_detection.lua` — Extension entry point script.

## Installation & Deployment Note

> [!IMPORTANT]
> - Both `idle_detection.lua` and the compiled dynamic library (`HPR_Idle_Detection_Extension.dll` on Windows or `HPR_Idle_Detection_Extension.so` on Linux) **must reside in the exact same folder** inside your extensions directory.
> - The filename of `HPR_Idle_Detection_Extension.dll` / `HPR_Idle_Detection_Extension.so` **must be kept exactly as named in the release** without renaming.

Placement directories:
- **Windows:** `%APPDATA%\HPR\HPR_Config\extensions\`
- **Linux:** `~/.config/HPR/HPR_Config/extensions/`

## Configuring Idle Time Threshold

Idle time is configured via `idle_detection_config.csv` in the extension directory alongside `idle_detection.lua`.

### CSV File Format
HPR expects a simple key-value CSV format where the threshold value is specified in **milliseconds**:

```csv
idle-threshold,<time_in_milliseconds>
```

### Conversion Examples
- **1 Minute:** `idle-threshold,60000`
- **5 Minutes:** `idle-threshold,300000`
- **8 Minutes (Default):** `idle-threshold,480000`
- **10 Minutes:** `idle-threshold,600000`
- **15 Minutes:** `idle-threshold,900000`

> [!TIP]
> If `idle_detection_config.csv` is missing on launch, the extension will automatically create it with the default 8-minute threshold (`480000` ms).

## How It Works

1. **Initialization:** On startup, `idle_detection.lua` resolves its absolute directory via `HPR.getExtensionAbsoluteDir()`, then dynamically loads `HPR_Idle_Detection_Extension.dll` (or `.so`) using `package.loadlib()` to expose native system idle detection (`getIdleStatus`).
2. **Config Management:** Reads `idle_detection_config.csv` for `idle-threshold` (defaulting to 8 minutes / `480000` ms if missing).
3. **System Monitoring:** On every tick (`onTick`), it checks user idle time against the configured threshold.
4. **Auto Pause & Resume:**
   - If idle time exceeds the threshold and the active window title is not ignored (e.g. YouTube), tracking pauses automatically via `HPR.stopTracking()`.
   - When user activity resumes, tracking automatically starts again via `HPR.startTracking()`.

### Idle Detection Backends

The native library selects the appropriate idle detection method at runtime based on the current platform and desktop environment:

| Platform | Backend | How it works |
| :--- | :--- | :--- |
| **Windows** | `GetLastInputInfo` (Win32 API) | Queries the timestamp of the last input event directly from the OS |
| **Linux — GNOME** | `org.gnome.Mutter.IdleMonitor` (D-Bus) | Calls the Mutter compositor's D-Bus `GetIdletime` method |
| **Linux — other Wayland** | `ext-idle-notify-v1` (Wayland protocol) | Subscribes to compositor idle/resumed events in a background thread; elapsed time since the `idled` event is the idle duration |

The Wayland backend works with any compositor that implements the standard `ext-idle-notify-v1` protocol, including **KDE/KWin**, **Hyprland**, **Sway**, **niri**, and others.

> [!WARNING]
> **X11 sessions are not supported.** If you are running a desktop on X11 (e.g. KDE on X11, Openbox, i3 on X), the extension will load successfully but idle detection will always return "not idle" and tracking will never be paused automatically. Only Wayland sessions are supported on non-GNOME Linux desktops.

## Build Dependencies

To compile the extension from source, you need a C++23 compatible compiler (GCC 13+, Clang 16+, or MSVC 2022), CMake (>= 3.15), and `pkg-config`.

### Linux Dependencies

The following libraries are needed on Linux. **Both** are required to enable full idle detection coverage (GNOME D-Bus + Wayland):

| Library | Purpose |
| :--- | :--- |
| `libglib2.0-dev` / `glib2-devel` | GLib / GIO — GNOME D-Bus idle detection |
| `libwayland-dev` / `wayland-devel` | Wayland client — non-GNOME Wayland idle detection |
| `wayland-protocols` | Protocol XML files for `ext-idle-notify-v1` |

Install them for your distribution:

- **Ubuntu / Debian / Linux Mint:**
  ```bash
  sudo apt install build-essential cmake pkg-config libglib2.0-dev libwayland-dev wayland-protocols
  ```
- **Fedora / RHEL / CentOS:**
  ```bash
  sudo dnf install gcc-c++ cmake pkgconfig glib2-devel wayland-devel wayland-protocols
  ```
- **Arch Linux / Manjaro:**
  ```bash
  sudo pacman -S base-devel cmake pkgconf glib2 wayland wayland-protocols
  ```
- **openSUSE:**
  ```bash
  sudo zypper install gcc-c++ cmake pkg-config glib2-devel wayland-devel wayland-protocols-devel
  ```
- **Alpine Linux:**
  ```bash
  sudo apk add build-base cmake pkgconf glib-dev wayland-dev wayland-protocols
  ```

> [!NOTE]
> If `wayland-client` or `wayland-protocols` are not detected at configure time, the Wayland idle backend is automatically disabled and only GNOME D-Bus detection will be compiled in. A message is printed by CMake: `Wayland ext-idle-notify-v1 support enabled` or `Wayland idle detection disabled`.

### Windows Dependencies
- **Visual Studio 2022 / MSVC** (with C++ Desktop Development workload)
- **CMake** (3.15+)

## Build Instructions

Using CMake:

```bash
mkdir build
cd build
cmake ..
cmake --build . --config Release
```
