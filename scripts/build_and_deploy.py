#!/usr/bin/env python3
"""Cross-platform build and deploy helper for the OBS mouse trail plugin."""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path


def load_env_file(path: Path) -> dict[str, str]:
    env: dict[str, str] = {}
    if not path.exists():
        raise FileNotFoundError(f"Missing .env file: {path}")

    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip().strip('"').strip("'")
        env[key] = os.path.expandvars(value)

    return env


def run(cmd: list[str], cwd: Path) -> None:
    print("+", " ".join(cmd))
    subprocess.run(cmd, cwd=str(cwd), check=True)


def resolve_obs_paths(obs_root: Path, os_name: str) -> tuple[Path, Path]:
    if os_name == "Darwin":
        if obs_root.suffix.lower() == ".app":
            contents = obs_root / "Contents"
        elif (obs_root / "Contents").exists():
            contents = obs_root / "Contents"
        else:
            contents = obs_root

        plugin_dir = contents / "PlugIns"
        locale_dir = contents / "Resources" / "data" / "obs-plugins" / "mouse_trail" / "locale"
        return plugin_dir, locale_dir

    plugin_dir = obs_root / "obs-plugins" / "64bit"
    locale_dir = obs_root / "data" / "obs-plugins" / "mouse_trail" / "locale"
    return plugin_dir, locale_dir


def find_plugin_binary(build_dir: Path) -> Path:
    candidates = list(build_dir.rglob("mouse_trail.dll"))
    candidates += list(build_dir.rglob("mouse_trail.so"))
    candidates += list(build_dir.rglob("mouse_trail.dylib"))
    candidates += list(build_dir.rglob("mouse_trail.bundle"))

    if not candidates:
        raise FileNotFoundError("Could not find built plugin binary in build directory")

    candidates.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    return candidates[0]


def copy_windows_runtime_dlls(obs_root: Path, mingw_bin: Path) -> None:
    runtime_dir = obs_root / "bin" / "64bit"
    runtime_dir.mkdir(parents=True, exist_ok=True)

    runtime_names = [
        "libgcc_s_seh-1.dll",
        "libstdc++-6.dll",
        "libwinpthread-1.dll",
    ]

    for name in runtime_names:
        src = mingw_bin / name
        dst = runtime_dir / name
        if src.exists():
            shutil.copy2(src, dst)
            print(f"copied runtime: {dst}")
        else:
            print(f"warning: runtime not found, skipping: {src}")


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    env_path = repo_root / ".env"

    try:
        env = load_env_file(env_path)
    except FileNotFoundError as exc:
        print(str(exc))
        return 1

    obs_root_value = env.get("OBS_ROOT", "").strip()
    if not obs_root_value:
        print("OBS_ROOT is required in .env")
        return 1

    obs_root = Path(obs_root_value)
    build_dir = repo_root / env.get("BUILD_DIR", "build")
    cmake_build_type = env.get("CMAKE_BUILD_TYPE", "Release")
    cmake_generator = env.get("CMAKE_GENERATOR", "").strip()
    cmake_build_config = env.get("CMAKE_BUILD_CONFIG", "").strip()

    configure_cmd = [
        "cmake",
        "-S",
        str(repo_root),
        "-B",
        str(build_dir),
        f"-DOBS_ROOT={obs_root}",
        f"-DCMAKE_BUILD_TYPE={cmake_build_type}",
        "-DMOUSE_TRAIL_DEPLOY_AFTER_BUILD=OFF",
    ]

    if cmake_generator:
        configure_cmd.extend(["-G", cmake_generator])

    for key in ("OBS_HEADERS_DIR", "OBS_CONFIG_DIR", "OBS_LIB_PATH"):
        value = env.get(key, "").strip()
        if value:
            configure_cmd.append(f"-D{key}={value}")

    build_cmd = ["cmake", "--build", str(build_dir)]
    if cmake_build_config:
        build_cmd.extend(["--config", cmake_build_config])

    try:
        run(configure_cmd, repo_root)
        run(build_cmd, repo_root)
    except subprocess.CalledProcessError as exc:
        print(f"build failed with exit code {exc.returncode}")
        return exc.returncode

    try:
        binary_path = find_plugin_binary(build_dir)
    except FileNotFoundError as exc:
        print(str(exc))
        return 1

    os_name = platform.system()
    plugin_dir, locale_dir = resolve_obs_paths(obs_root, os_name)
    plugin_dir.mkdir(parents=True, exist_ok=True)
    locale_dir.mkdir(parents=True, exist_ok=True)

    dst_plugin = plugin_dir / binary_path.name
    dst_locale = locale_dir / "en-US.ini"

    try:
        shutil.copy2(binary_path, dst_plugin)
        shutil.copy2(repo_root / "data" / "locale" / "en-US.ini", dst_locale)
    except PermissionError:
        print("permission denied while copying files. Close OBS and retry.")
        return 1

    if os_name == "Windows":
        mingw_bin = Path(env.get("MINGW_BIN", "C:/Strawberry/c/bin"))
        copy_windows_runtime_dlls(obs_root, mingw_bin)

    print("build and deploy completed")
    print(f"plugin: {dst_plugin}")
    print(f"locale: {dst_locale}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
