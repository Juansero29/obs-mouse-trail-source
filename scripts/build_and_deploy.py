#!/usr/bin/env python3
"""Cross-platform build and deploy helper for the OBS mouse trail plugin."""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

try:
    from rich.console import Console
    from rich.panel import Panel
    from rich.progress import BarColumn
    from rich.progress import Progress
    from rich.progress import SpinnerColumn
    from rich.progress import TaskProgressColumn
    from rich.progress import TextColumn
    from rich.progress import TimeElapsedColumn
except ImportError:
    print("Missing dependency: rich")
    print("Install it with: python -m pip install rich")
    sys.exit(2)


class BuildScriptError(Exception):
    """Expected script failures with actionable messages."""

    def __init__(
        self,
        message: str,
        *,
        returncode: int = 1,
        cmd: list[str] | None = None,
        stdout: str = "",
        stderr: str = "",
    ) -> None:
        super().__init__(message)
        self.message = message
        self.returncode = returncode
        self.cmd = cmd or []
        self.stdout = stdout
        self.stderr = stderr


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


def run_command_with_progress(
    progress: Progress,
    task_id: int,
    console: Console,
    cmd: list[str],
    cwd: Path,
    label: str,
) -> None:
    progress.update(task_id, description=f"[cyan]{label}", completed=5)

    result_holder: dict[str, subprocess.CompletedProcess[str]] = {}

    def worker() -> None:
        result_holder["result"] = subprocess.run(
            cmd,
            cwd=str(cwd),
            text=True,
            capture_output=True,
            check=False,
        )

    thread = threading.Thread(target=worker, daemon=True)
    thread.start()

    current = 5
    while thread.is_alive():
        current = 95 if current >= 95 else current + 1
        progress.update(task_id, completed=current)
        time.sleep(0.08)

    thread.join()
    result = result_holder["result"]

    if result.returncode != 0:
        raise BuildScriptError(
            f"{label} failed",
            returncode=result.returncode,
            cmd=cmd,
            stdout=result.stdout,
            stderr=result.stderr,
        )

    progress.update(task_id, completed=100)
    console.print(f"[green]OK[/green] {label}")


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


def copy_windows_runtime_dlls(obs_root: Path, mingw_bin: Path, console: Console) -> None:
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
            console.print(f"[green]runtime copied[/green] {dst}")
        else:
            console.print(f"[yellow]warning[/yellow] runtime not found, skipped {src}")


def normalize_error_message(err: BuildScriptError) -> str:
    combined = f"{err.stdout}\n{err.stderr}".lower()
    if "being used by another process" in combined:
        return "OBS is probably still running and locking plugin files. Close OBS and retry."
    if "permission denied" in combined or "access is denied" in combined:
        return "Permission denied while deploying files. Close OBS and run with sufficient permissions."
    if "cmake" in combined and "not recognized" in combined:
        return "CMake was not found in PATH. Install CMake and retry."
    return err.message


def print_error_details(console: Console, err: BuildScriptError) -> None:
    message = normalize_error_message(err)
    console.print(Panel.fit(f"[bold red]Build Failed[/bold red]\n{message}", border_style="red"))

    if err.cmd:
        console.print("[red]Command:[/red] " + " ".join(err.cmd))

    output = (err.stderr or err.stdout or "").strip()
    if output:
        tail = "\n".join(output.splitlines()[-25:])
        console.print("[red]Last output lines:[/red]")
        console.print(tail)


def main() -> int:
    console = Console()
    repo_root = Path(__file__).resolve().parents[1]
    env_path = repo_root / ".env"

    console.print(
        Panel.fit(
            "[bold cyan]OBS Mouse Trail Build & Deploy[/bold cyan]\n"
            "Cross-platform builder with rich progress output",
            border_style="cyan",
        )
    )

    try:
        env = load_env_file(env_path)
    except FileNotFoundError as exc:
        console.print(f"[red]{exc}[/red]")
        return 1

    obs_root_value = env.get("OBS_ROOT", "").strip()
    if not obs_root_value:
        console.print("[red]OBS_ROOT is required in .env[/red]")
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

    steps = [
        "Load configuration",
        "Configure CMake",
        "Build plugin",
        "Locate plugin binary",
        "Prepare OBS destination",
        "Deploy plugin binary",
        "Deploy locale",
        "Deploy runtimes (Windows)",
    ]

    try:
        with Progress(
            SpinnerColumn(style="cyan"),
            TextColumn("[progress.description]{task.description}"),
            BarColumn(bar_width=30),
            TaskProgressColumn(),
            TimeElapsedColumn(),
            console=console,
            transient=False,
        ) as progress:
            step1 = progress.add_task(f"[cyan]{steps[0]}", total=100)
            progress.update(step1, completed=100)

            step2 = progress.add_task(f"[cyan]{steps[1]}", total=100)
            run_command_with_progress(progress, step2, console, configure_cmd, repo_root, steps[1])

            step3 = progress.add_task(f"[cyan]{steps[2]}", total=100)
            run_command_with_progress(progress, step3, console, build_cmd, repo_root, steps[2])

            step4 = progress.add_task(f"[cyan]{steps[3]}", total=100)
            try:
                binary_path = find_plugin_binary(build_dir)
            except FileNotFoundError as exc:
                raise BuildScriptError(str(exc)) from exc
            progress.update(step4, completed=100)
            console.print(f"[green]OK[/green] {steps[3]}: {binary_path}")

            step5 = progress.add_task(f"[cyan]{steps[4]}", total=100)
            os_name = platform.system()
            plugin_dir, locale_dir = resolve_obs_paths(obs_root, os_name)
            plugin_dir.mkdir(parents=True, exist_ok=True)
            locale_dir.mkdir(parents=True, exist_ok=True)
            progress.update(step5, completed=100)

            dst_plugin = plugin_dir / binary_path.name
            dst_locale = locale_dir / "en-US.ini"

            step6 = progress.add_task(f"[cyan]{steps[5]}", total=100)
            try:
                shutil.copy2(binary_path, dst_plugin)
            except PermissionError as exc:
                raise BuildScriptError("Failed to deploy plugin binary", stderr=str(exc)) from exc
            progress.update(step6, completed=100)

            step7 = progress.add_task(f"[cyan]{steps[6]}", total=100)
            try:
                shutil.copy2(repo_root / "data" / "locale" / "en-US.ini", dst_locale)
            except PermissionError as exc:
                raise BuildScriptError("Failed to deploy locale file", stderr=str(exc)) from exc
            progress.update(step7, completed=100)

            step8 = progress.add_task(f"[cyan]{steps[7]}", total=100)
            if os_name == "Windows":
                mingw_bin = Path(env.get("MINGW_BIN", "C:/Strawberry/c/bin"))
                try:
                    copy_windows_runtime_dlls(obs_root, mingw_bin, console)
                except PermissionError as exc:
                    raise BuildScriptError("Failed to deploy runtime DLLs", stderr=str(exc)) from exc
            progress.update(step8, completed=100)

    except BuildScriptError as err:
        print_error_details(console, err)
        return err.returncode

    console.print(Panel.fit("[bold green]Build and deploy completed successfully[/bold green]", border_style="green"))
    console.print(f"[green]Plugin:[/green] {dst_plugin}")
    console.print(f"[green]Locale:[/green] {dst_locale}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
