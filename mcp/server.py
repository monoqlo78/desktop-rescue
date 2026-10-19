"""Optional official-SDK MCP adapter. The resident has no Python dependency."""

import json
import os
from pathlib import Path
import subprocess

from mcp.server.fastmcp import FastMCP

server = FastMCP("desktop-rescue")
COMMANDS = frozenset({"start", "stop", "status", "recover"})


def execute(command: str) -> dict:
    if command not in COMMANDS:
        raise ValueError("Unsupported desktop-rescue command")
    configured = os.environ.get("DESKTOP_RESCUE_EXE")
    executable = (
        Path(configured) if configured else Path(__file__).resolve().parent.parent / "desktop-rescue.exe"
    )
    if not executable.is_absolute() or not executable.is_file():
        raise ValueError("DESKTOP_RESCUE_EXE must name an existing absolute executable path")
    completed = subprocess.run(
        [str(executable), command],
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=12,
        check=False,
        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
    )
    try:
        result = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"desktop-rescue returned invalid JSON (exit {completed.returncode})") from error
    if not isinstance(result, dict) or not isinstance(result.get("ok"), bool):
        raise RuntimeError("desktop-rescue returned an invalid result")
    if completed.returncode != 0 or not result["ok"]:
        raise RuntimeError(json.dumps(result, ensure_ascii=False))
    return result


@server.tool()
def start() -> dict:
    """Start the independent resident on the normal Default input desktop."""
    return execute("start")


@server.tool()
def status() -> dict:
    """Read resident health, desktop diagnostics and process resource usage."""
    return execute("status")


@server.tool()
def recover() -> dict:
    """Attempt recovery to the fixed startup desktop and verify the input desktop."""
    return execute("recover")


@server.tool()
def stop() -> dict:
    """Stop only desktop-rescue in this logon session; do not change auto-start."""
    return execute("stop")


if __name__ == "__main__":
    server.run(transport="stdio")
