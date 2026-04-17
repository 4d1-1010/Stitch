#!/usr/bin/env python3
"""
Stitch — Server entry point.

Usage:
    python run_server.py [--host HOST] [--port PORT] [--config CONFIG]
"""

import argparse
import asyncio
import logging
import os
import re
import signal
import socket
import subprocess
import sys
import yaml
from pathlib import Path
from typing import Optional

# Allow running this script directly without installing the package.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from stitch.apps.server import Server
from stitch.apps.client import Client

log = logging.getLogger("run_server")

PROJECT_ROOT = Path(__file__).resolve().parent.parent


def load_config(path: str) -> dict:
    p = Path(path)
    if not p.exists():
        return {}
    with open(p) as f:
        return yaml.safe_load(f) or {}


def parse_layout_offsets(config: dict) -> dict:
    """Parse layout.machines from config into offset dict."""
    offsets = {}
    machines = config.get("layout", {}).get("machines", {})
    for mid, mconf in machines.items():
        if "offset_x" in mconf and "offset_y" in mconf:
            offsets[mid] = (mconf["offset_x"], mconf["offset_y"])
    return offsets


def default_local_id() -> str:
    """Sanitized hostname for use as a machine_id."""
    raw = socket.gethostname() or "server"
    clean = re.sub(r"[^A-Za-z0-9_-]", "-", raw).strip("-")
    return clean or "server"


def launch_configurator(port: int) -> Optional[subprocess.Popen]:
    """Spawn the tkinter configurator as a subprocess.

    Returns the Popen handle or None if the process couldn't be started
    (e.g. no display available, tkinter missing).
    """
    script = Path(__file__).resolve().parent / "run_configurator.py"
    cmd = [sys.executable, str(script), "--server", "127.0.0.1",
           "--port", str(port)]
    try:
        proc = subprocess.Popen(cmd, cwd=str(PROJECT_ROOT))
        log.info("Launched configurator UI (pid=%d)", proc.pid)
        return proc
    except OSError as e:
        log.warning("Could not launch configurator (%s). Server runs "
                    "without UI — use --no-ui to silence this.", e)
        return None


async def run_local_client(machine_id: str, port: int) -> None:
    """Run an in-process client connected to the local server.

    Degrades gracefully: if backend init fails (no display, missing perms),
    logs a warning and leaves the server running in headless mode.
    """
    client = Client(machine_id=machine_id, server_host="127.0.0.1",
                    server_port=port)
    try:
        await client.run()
    except asyncio.CancelledError:
        raise
    except (OSError, RuntimeError) as e:
        log.warning("Local client disabled (%s: %s). Server continues "
                    "without local displays.", type(e).__name__, e)
    finally:
        try:
            await client.stop()
        except OSError as e:
            log.debug("Local client shutdown error: %s", e)


def main():
    parser = argparse.ArgumentParser(description="Stitch Server")
    parser.add_argument("--host", default="0.0.0.0", help="Bind address")
    parser.add_argument("--port", type=int, default=24800, help="TCP port")
    parser.add_argument("--no-ui", action="store_true",
                        help="Don't launch the configurator UI")
    parser.add_argument("--no-local-client", action="store_true",
                        help="Don't include the server machine's displays "
                             "(headless server mode)")
    parser.add_argument("--local-id", default=None,
                        help="Machine ID for the server's own displays "
                             "(default: hostname)")
    parser.add_argument("--config", default="config.yaml", help="Config file")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    )

    config = load_config(args.config)
    offsets = parse_layout_offsets(config)

    server = Server(
        host=args.host,
        port=args.port,
        layout_offsets=offsets or None,
    )

    local_id = args.local_id or default_local_id()
    configurator_proc: Optional[subprocess.Popen] = None

    async def run_all():
        nonlocal configurator_proc
        await server.start()

        local_task = None
        if not args.no_local_client:
            log.info("Starting local client as '%s'", local_id)
            local_task = asyncio.create_task(
                run_local_client(local_id, args.port),
                name="local-client",
            )

        if not args.no_ui:
            configurator_proc = launch_configurator(args.port)

        try:
            await server.serve_forever()
        finally:
            if local_task and not local_task.done():
                local_task.cancel()
                try:
                    await local_task
                except asyncio.CancelledError:
                    pass

    loop = asyncio.new_event_loop()

    def shutdown():
        loop.create_task(server.stop())
        loop.call_later(1, loop.stop)

    if sys.platform != "win32":
        for sig in (signal.SIGINT, signal.SIGTERM):
            loop.add_signal_handler(sig, shutdown)

    try:
        loop.run_until_complete(run_all())
    except KeyboardInterrupt:
        pass
    finally:
        if configurator_proc and configurator_proc.poll() is None:
            configurator_proc.terminate()
            try:
                configurator_proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                configurator_proc.kill()
        loop.run_until_complete(server.stop())
        loop.close()


if __name__ == "__main__":
    main()
