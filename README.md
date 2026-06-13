# OBS Dynamic Mouse Trail Source

A custom OBS Studio input source plugin that renders a smooth, fading mouse trail.

## Quick Install (No Build Tools)

You do not need to build from source to use this plugin.

1. Open the repo Releases page on GitHub.
2. Download the latest `mouse-trail-windows-x64-<version>.zip`.
3. Close OBS completely.
4. Extract the ZIP into your OBS install root.
   - Example: `C:\Program Files\obs-studio`
5. Start OBS and add source -> **Dynamic Mouse Trail**.

The release ZIP is already laid out with the folders OBS expects:

- `obs-plugins/64bit/mouse_trail.dll`
- `data/obs-plugins/mouse_trail/locale/en-US.ini`
- `bin/64bit/*.dll` (required MinGW runtime DLLs)

## Maintainer Release Flow (Tags)

This repository includes an automated release workflow at `.github/workflows/release.yml`.

1. Update version if needed.
2. Commit and push to main.
3. Create and push a version tag:

```powershell
git tag v1.0.0
git push origin v1.0.0
```

4. GitHub Actions will build, package, and publish a GitHub Release automatically.

## Features

- Smooth mouse trail rendering
- Age-based fade-out (newer points are brighter)
- Selectable trail behavior modes:
   - Smoke mode: upward drift (anti-gravity effect) on older points
   - Precision mode: no drift, trail fades in-place
- Configurable trail color in source properties
- **Canvas Mapping Modes** (new):
   - All Monitors: trail canvas spans all connected monitors
   - Specific Monitor: trail canvas limited to a selected monitor
   - Mousepad: trail movement mapped based on mouse DPI and physical mousepad size
- **Boundary Behaviors** (new):
   - Delimited Zone: trail clamped at canvas edges (stops at boundary)
   - Infinite Wrap: trail wraps to opposite side when crossing boundary
   - Infinite Recenter: trail jumps back toward center when reaching boundary
- Thread-safe trail management
- Frame-rate-independent updates using OBS `video_tick` delta time
- Cross-platform support (Windows primary, macOS/Linux fallback)

## Repository Layout

- `src/plugin-main.c`: OBS module entry points
- `src/trail-source.cpp`: source callbacks (`create`, `video_tick`, `video_render`)
- `src/trail-manager.hpp`: thread-safe trail data structure + physics
- `data/locale/en-US.ini`: locale file for the plugin data path
- `CMakeLists.txt`: standalone MinGW build + OBS deployment copy steps
- `.env`: local build/deploy settings (OBS path and optional overrides)
- `scripts/build_and_deploy.py`: cross-platform build + deploy script
- `scripts/build_and_deploy.ps1`: Windows PowerShell wrapper
- `scripts/build_and_deploy.cmd`: Windows Command Prompt wrapper
- `scripts/build_and_deploy.sh`: macOS/Linux shell wrapper

## Cross-Platform Build And Deploy (.env + Script)

This repository includes a single Python script that runs on Windows, macOS, and Linux:

- `scripts/build_and_deploy.py`

It does three things:

1. Configures CMake with your `.env` values
2. Builds the plugin
3. Copies the plugin and locale files into your OBS installation

### 1) Configure `.env`

Edit `.env` and set at least:

- `OBS_ROOT`: root of your OBS installation

Examples:

- Windows: `OBS_ROOT=D:/Programs/OBS28/obs-studio`
- Linux: `OBS_ROOT=/usr`
- macOS: `OBS_ROOT=/Applications/OBS.app`

Optional settings in `.env`:

- `BUILD_DIR` (default: `build`)
- `CMAKE_BUILD_TYPE` (default: `Release`)
- `CMAKE_GENERATOR` and `CMAKE_BUILD_CONFIG`
- `OBS_HEADERS_DIR`, `OBS_CONFIG_DIR`, `OBS_LIB_PATH`
- `MINGW_BIN` (Windows runtime DLL copy source)

### 2) Run The Script

From repo root:

```powershell
python scripts/build_and_deploy.py
```

On macOS/Linux you can also use:

```bash
python3 scripts/build_and_deploy.py
```

Or use the wrappers:

```powershell
./scripts/build_and_deploy.ps1
```

```bat
scripts\build_and_deploy.cmd
```

```bash
bash scripts/build_and_deploy.sh
```

### 3) Restart OBS And Test

1. Close OBS before running the script (to avoid file locks)
2. Run the script
3. Start OBS and add **Dynamic Mouse Trail**
4. Test **Trail Mode** and **Trail Color** in source properties

## Source Properties Guide

### Trail Mode
- **Smoke**: older trail points drift upward (anti-gravity effect). Useful for attention-grabbing effects.
- **Precision**: trail points fade in place without upward drift. Better for precise cursor tracking.

### Trail Color
Set the color of the trail gradient. Points fade from this color to transparent.

### Canvas Mode
Defines the coordinate system for the trail canvas:
- **All Monitors**: trail spans across all connected monitors (virtual desktop). Default mode.
- **Specific Monitor**: trail is limited to a single monitor. Select which monitor from the dropdown after choosing this option.
- **Mousepad**: trail movement is based on physical mousepad dimensions and mouse DPI. See mousepad settings below.

### Monitor Selection (only visible when "Specific Monitor" is selected)
Dropdown list of available monitors with their resolution. Format: `DISPLAY1 (1920x1080 @ 0,0)`

### Mousepad Settings (only visible when "Mousepad" mode is selected)
- **Mouse DPI**: sensitivity of your mouse (dots per inch). Default: 800 DPI. Typical range: 400-3200.
- **Mousepad Width**: physical width of your mousepad in millimeters. Default: 250mm.
- **Mousepad Height**: physical height of your mousepad in millimeters. Default: 250mm.

When in mousepad mode, the trail movement is calculated based on actual mouse movement (in screen pixels converted to physical distance) divided by your mousepad dimensions. This allows the trail to represent movement relative to your physical input device.

### Boundary Behavior
Controls what happens when the trail reaches the edge of the canvas:
- **Delimited Zone (Clamped)**: trail stops at the canvas boundary. Movement beyond the boundary is not visible.
- **Infinite Wrap (Continuous)**: when the trail crosses an edge, it continues from the opposite side (wraps around).
- **Infinite Recenter (Jump to Center)**: when the trail crosses an edge, it jumps back toward the center and continues. Creates a "teleport" effect at boundaries.

## Prerequisites (Windows, MinGW path)

This section is only required if you want to build or modify the plugin.

1. OBS Studio installed
2. CMake 3.16+
3. Ninja
4. Strawberry Perl MinGW toolchain (`gcc`, `g++`, `gendef`, `dlltool`)

## One-Time Setup

This lightweight build links against your installed OBS and a local copy of OBS headers.

1. Create dependency folders:

```powershell
mkdir .deps\lib -Force
mkdir .deps\tmp -Force
```

2. Download OBS source headers (version should match your installed OBS as closely as possible):

```powershell
Invoke-WebRequest -Uri "https://github.com/obsproject/obs-studio/archive/refs/tags/31.1.1.zip" -OutFile .deps\obs-studio.zip
Expand-Archive -Path .deps\obs-studio.zip -DestinationPath .deps
Rename-Item .deps\obs-studio-31.1.1 .deps\obs-studio-src
```

3. Generate MinGW import library from your installed `obs.dll`:

```powershell
Copy-Item "C:\Program Files\obs-studio\bin\64bit\obs.dll" .deps\tmp\obs.dll -Force
Push-Location .deps\tmp
gendef obs.dll
dlltool -d obs.def -l ..\lib\libobs.dll.a --dllname obs.dll
Pop-Location
```

4. Ensure `obsconfig.h` exists at:

- `.deps/obs-studio-src/build_x64/config/obsconfig.h`

If it is missing, copy it from an existing OBS source build tree or generate it with an OBS CMake configure.

## Build

1. Set your OBS installation root (adjust path if needed):

```powershell
cmake -S . -B build -G Ninja -DOBS_ROOT="D:/Programs/OBS28/obs-studio"
```

2. Build:

```powershell
cmake --build build
```

On successful build, CMake post-build steps will copy:

- `mouse_trail.dll` -> `<OBS_ROOT>/obs-plugins/64bit/`
- locale file -> `<OBS_ROOT>/data/obs-plugins/mouse_trail/locale/`
- MinGW runtime DLLs -> `<OBS_ROOT>/bin/64bit/`

## Install / Verify in OBS

1. Close OBS before copying/rebuilding (to avoid file lock errors).
2. Launch OBS.
3. Add source -> **Dynamic Mouse Trail**.
4. Move your mouse and verify the trail appears.
5. Open the source properties to set **Trail Mode** and **Trail Color**.

If the source does not appear, check:

```powershell
$log = Get-ChildItem "$env:APPDATA\obs-studio\logs" -Filter "*.txt" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Select-String -Path $log.FullName -Pattern "mouse_trail|Failed to initialize module|obs_register_source"
```

## Contributing

Contributions are welcome.

1. Fork the repo and create a feature branch.
2. Keep changes focused and include a short rationale in the PR description.
3. Verify:
   - project builds with your configured `OBS_ROOT`
   - source appears in OBS Add Source menu
   - trail renders without regressions
4. Run formatting/linting tools you normally use for C/C++ before opening a PR.

### Suggested PR Checklist

- [ ] Builds successfully
- [ ] OBS loads `mouse_trail.dll`
- [ ] Source registration succeeds (no `obs_register_source failed`)
- [ ] Visual behavior tested in OBS preview
- [ ] README updated if setup or behavior changed

## Platform Support

### Windows (Primary)
- ✅ Full support for all features
- Monitor enumeration via `EnumDisplayMonitors`
- Cursor position via `GetCursorPos`
- Multi-monitor support with individual monitor selection

### macOS/Linux
- ⚠️ Limited support (fallback mode)
- Mousepad mode not yet implemented (uses approximation)
- Monitor selection shows single fallback monitor
- Contributions welcome for platform-specific implementations

### Known Limitations
- **Mousepad mode**: Currently only implemented on Windows. macOS/Linux use a simple screen-space fallback.
- **Monitor enumeration**: Platform-specific. Windows is fully featured.
- **Boundary behaviors**: All modes (wrap, recenter, delimited) work consistently across platforms.

## Troubleshooting

### Source doesn't appear in OBS
Check the OBS log:
```powershell
$log = Get-ChildItem "$env:APPDATA\obs-studio\logs" -Filter "*.txt" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Select-String -Path $log.FullName -Pattern "mouse_trail|Failed to initialize module"
```

### Trail not visible or frozen
1. Ensure **Trail Mode** and **Trail Color** are set in source properties
2. Move your mouse in the OBS preview area to trigger trail updates
3. Check that the selected canvas mode is appropriate for your setup

### Monitor selection shows empty list
This typically means the monitor enumeration encountered an issue. Try:
1. Use **All Monitors** mode as default
2. Restart OBS
3. Check the OBS log for monitor enumeration errors

## License

This project is licensed under GPLv2-or-later. See `LICENSE`.
