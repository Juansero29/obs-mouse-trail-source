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
- Upward drift (anti-gravity effect) on older points
- Thread-safe trail management
- Frame-rate-independent updates using OBS `video_tick` delta time

## Repository Layout

- `src/plugin-main.c`: OBS module entry points
- `src/trail-source.cpp`: source callbacks (`create`, `video_tick`, `video_render`)
- `src/trail-manager.hpp`: thread-safe trail data structure + physics
- `data/locale/en-US.ini`: locale file for the plugin data path
- `CMakeLists.txt`: standalone MinGW build + OBS deployment copy steps

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

## License

This project is licensed under GPLv2-or-later. See `LICENSE`.
