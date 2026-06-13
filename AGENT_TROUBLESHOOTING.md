# Agent Troubleshooting Reference

This document captures lessons learned while building and debugging this OBS plugin.

## Goal of This File

Use this as a fast path when the plugin:

- does not appear in OBS
- appears but cannot be added
- is added but nothing renders
- builds but deployment fails

## High-Value Lessons Learned

1. The active OBS installation path matters more than expected.
- If OBS is running from a non-default path (for example `D:/Programs/OBS28/obs-studio`), deploying to `C:/Program Files/obs-studio` will silently fail from the user's perspective.
- Always verify the real running path first.

2. OBS module loading depends on a three-part deployment set.
- Plugin DLL: `obs-plugins/64bit/mouse_trail.dll`
- Plugin data: `data/obs-plugins/mouse_trail/locale/en-US.ini`
- MinGW runtimes (when MinGW-built): `bin/64bit/libgcc_s_seh-1.dll`, `bin/64bit/libstdc++-6.dll`, `bin/64bit/libwinpthread-1.dll`

3. Source registration can fail even when module loads.
- Missing callbacks like `get_width` and `get_height` can cause registration failure.
- Keep `OBS_SOURCE_CUSTOM_DRAW` in output flags so render callback is invoked.

4. Build success does not mean deployment success.
- OBS can lock plugin files while running.
- Post-build copy may fail until OBS is closed.

5. Rendering invisibility is usually one of three causes.
- Coordinates mapped outside the OBS canvas.
- Render callback runs but geometry/effect path is wrong.
- Source is behind other scene items or transform/crop hides output.

## Rapid Triage Checklist

1. Confirm active OBS path.
- Verify where `obs64.exe` is running from.
- Ensure `OBS_ROOT` matches that exact install root.

2. Confirm files exist in that same root.
- `obs-plugins/64bit/mouse_trail.dll`
- `data/obs-plugins/mouse_trail/locale/en-US.ini`
- required runtime DLLs in `bin/64bit`

3. Confirm OBS can load the module.
- Check latest OBS log for `mouse_trail` load lines and init failures.

4. Confirm source registration and scene addition.
- Look for registration success or failure in logs.
- Confirm source appears in Add Source menu.

5. Confirm tick and render execution.
- If needed, temporarily add throttled logging in tick/render.
- Remove verbose logs after diagnosis.

## Useful Commands (PowerShell)

Find latest OBS log and filter relevant lines:

```powershell
$log = Get-ChildItem "$env:APPDATA\obs-studio\logs" -Filter "*.txt" |
  Sort-Object LastWriteTime -Descending |
  Select-Object -First 1

Select-String -Path $log.FullName -Pattern "mouse_trail|Failed to initialize module|obs_register_source"
```

Check where OBS is running from:

```powershell
Get-Process obs64 | Select-Object Id, Path
```

## Known Build/Link Pitfalls

1. Missing `obsconfig.h`
- This header is generated in OBS builds and is required by libobs headers.
- In this lightweight setup, expected location is `.deps/obs-studio-src/build_x64/config/obsconfig.h`.

2. Import library mismatch
- `libobs.dll.a` must match the target architecture and be generated from a compatible `obs.dll`.

3. Toolchain mismatch
- Keep architecture consistent (x64 all the way).
- Prefer one MinGW toolchain family throughout the build.

## Rendering Debug Notes

When "source exists but no trail appears":

1. Verify point count grows over time.
2. Verify mapped coordinates remain within base canvas dimensions.
3. Verify alpha/fade math does not clamp to zero too quickly.
4. Verify render pass and effect setup are valid before geometry submission.

## Agent Working Agreements

1. Make minimal, reversible changes during diagnosis.
2. Prefer throttled logs (for example every 1-2 seconds), not per-frame spam.
3. Remove temporary debug logs before finalizing unless user asks to keep them.
4. If changing deployment behavior, update both `README.md` and `AGENTS.md`.

## When to Escalate

Escalate to user confirmation when:

- multiple OBS installs are detected and active path is unclear
- OBS version mismatch may affect ABI behavior
- security software blocks DLL copy/load
- reproducibility depends on custom local environment state
