# AGENTS.md

This file helps any coding agent quickly understand and safely modify this repository.

## Project Summary

- Project type: OBS Studio plugin (Windows, C/C++)
- Plugin id: `mouse_trail_source`
- Output module: `mouse_trail.dll`
- Primary goal: Render a dynamic, fading mouse trail as an OBS source.

## Important Paths

- Source registration and module entry: `src/plugin-main.c`
- Source logic and rendering: `src/trail-source.cpp`
- Trail data model and physics: `src/trail-manager.hpp`
- Localized display name: `data/locale/en-US.ini`
- Build and deployment behavior: `CMakeLists.txt`
- Release automation: `.github/workflows/release.yml`
- Troubleshooting playbook: `AGENT_TROUBLESHOOTING.md`

## Build Model

This repository uses a lightweight MinGW build that links against OBS through:

- OBS headers under `.deps/obs-studio-src`
- Generated import library at `.deps/lib/libobs.dll.a`

CMake post-build copies artifacts into an OBS root structure:

- `obs-plugins/64bit/mouse_trail.dll`
- `data/obs-plugins/mouse_trail/locale/en-US.ini`
- required MinGW runtime DLLs into `bin/64bit`

## Safe-Change Rules for Agents

1. Preserve `mouse_trail_source` id unless explicitly requested.
2. Keep `OBS_SOURCE_CUSTOM_DRAW` in output flags; render path depends on it.
3. Keep `get_width` and `get_height` callbacks implemented.
4. Avoid introducing verbose runtime logging unless debugging is requested.
5. Keep rendering changes focused and testable (tick mapping + render output).
6. Do not hardcode machine-specific absolute paths except existing defaults that are user-overridable.

## Release Flow

- Tag format: `vX.Y.Z` (example: `v1.0.0`)
- Tag push triggers `.github/workflows/release.yml`
- Workflow builds on Windows and publishes a ZIP containing install-ready folders.

## Quick Validation Checklist

- Plugin compiles with CMake + Ninja.
- OBS loads module without initialization errors.
- Source appears as `Dynamic Mouse Trail` in Add Source menu.
- Trail renders and fades correctly in preview.

## Suggested Agent Task Sequence

1. Read `README.md` and this file.
2. If debugging, read `AGENT_TROUBLESHOOTING.md` before editing render/deploy logic.
3. Inspect current rendering path in `src/trail-source.cpp`.
4. Make minimal changes.
5. Keep README and release notes in sync if install/build behavior changes.
