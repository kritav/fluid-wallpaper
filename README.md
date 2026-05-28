# Fluid Wallpaper

Real-time CUDA fluid simulation as a Windows desktop wallpaper. The fluid reacts to your mouse, and the rest of the desktop (icons, windows, taskbar) keeps working normally on top of it.

[![Download](https://img.shields.io/github/v/release/kritav/fluid-wallpaper)](https://github.com/kritav/fluid-wallpaper/releases/latest)


![Fluid Wallpaper — velocity-colored mode reacting to the cursor](docs/demo.gif)

## Download

Get the latest installer from the [Releases page](https://github.com/kritav/fluid-wallpaper/releases) and run it. No admin rights needed; the installer drops the app into your per-user Programs folder.
(docs/Step1.png)
(docs/Step2.png)
(docs/Step3.png)

## Requirements

- Windows 10 or 11 (64-bit)
- NVIDIA GPU with CUDA support (Turing / RTX 20-series or newer recommended; Pascal works at reduced perf)
- ~50 MB disk, ~150 MB RAM at runtime

## Install

1. Download `fluid-wallpaper-setup-<version>.exe` from the Releases page.
2. Run it. You can optionally tick **Start with Windows** and **Create desktop shortcut** in the wizard.
3. The wallpaper starts at the end of install and a one-time welcome dialog points you at the tray icon.

To uninstall: **Settings → Apps → Installed apps → Fluid Wallpaper → Uninstall**. The autostart entry and app settings are removed cleanly.

## Use it

The tray icon (look for the fluid droplet near the clock) is the main control surface. Right-click it for:

- **Pause / Resume** — freezes the simulation; wallpaper stays visible
- **Clear simulation**
- **Next visual mode** — cycles through plasma, inferno, viridis, cool, velocity, velocity-colored
- **Bloom** — toggle the bloom post-process
- **Idle motion** — self-stir when the cursor is idle for >3 s
- **Start with Windows** — toggles the autostart registry entry
- **About** — version + GitHub link
- **Exit**

Keyboard shortcuts (also work without focusing any particular window):

| Key  | Action                       |
|------|------------------------------|
| `M`  | next visual mode             |
| `B`  | toggle bloom                 |
| `I`  | toggle idle motion           |
| `C`  | clear simulation             |
| `Esc`| quit                         |

## Performance

The app paces itself based on visibility and power source:

| State                                     | FPS  |
|-------------------------------------------|------|
| Default (visible, AC power)               | 60   |
| On battery                                | 30   |
| Desktop fully obscured by a window        | 5    |
| Fullscreen app running / monitor off / paused | 2 (heartbeat) |

GPU usage on an RTX 3060 in the default state is ~3–5%. When a fullscreen game is running it drops to effectively zero.

## Build from source

Prerequisites: Visual Studio 2022 (with Desktop C++), CUDA Toolkit 12+, CMake 3.20+.

```
cd wallpaper
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `wallpaper/build/Release/fluid-wallpaper.exe` (plus a `shaders/` subdir).

To produce the installer (requires [Inno Setup 6](https://jrsoftware.org/isinfo.php)):

```
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\fluid-wallpaper.iss
```

Output: `installer/output/fluid-wallpaper-setup-<version>.exe`.

To regenerate the placeholder icons (overwrites `wallpaper/resources/{app,tray}.ico`):

```
python3 wallpaper/resources/_gen_icons.py
```

## Troubleshooting

**Important** Make sure Windows Defender allows you to download the app in the first place!

**Nothing happens after install.** Open Task Manager → Details and confirm `fluid-wallpaper.exe` is running. If it's not, the most common cause is a missing CUDA runtime: install the latest NVIDIA driver.

**Wallpaper shows behind a black rectangle / icons disappear.** This is an interaction with virtual-desktop tools or third-party shell replacements. Try right-clicking the desktop → **View → Show desktop icons** to toggle.

**Tray icon missing.** Windows hides "rarely used" tray icons by default. Open the tray overflow (the `^` arrow) or pin it via **Settings → Personalization → Taskbar → Other system tray icons**.

**GPU usage seems high.** Check the tray menu — if "Pause" isn't checked and you're plugged in, you should be at ~60 FPS. On a laptop unplug-and-replug to confirm the battery detection is working (you should see GPU usage halve).

## License

MIT. See [LICENSE](LICENSE).
