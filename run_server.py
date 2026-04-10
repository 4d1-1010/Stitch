#!/usr/bin/env python3
"""
Unified Desktop — Server entry point.

Usage:
    python run_server.py [--host HOST] [--port PORT] [--config CONFIG]
"""

import argparse
import asyncio
import logging
import signal
import sys
import yaml
from pathlib import Path

from ud.server import Server


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


def main():
    parser = argparse.ArgumentParser(description="Unified Desktop Server")
    parser.add_argument("--host", default="0.0.0.0", help="Bind address")
    parser.add_argument("--port", type=int, default=24800, help="Port")
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

    loop = asyncio.new_event_loop()

    def shutdown():
        loop.create_task(server.stop())
        loop.call_later(1, loop.stop)

    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, shutdown)

    try:
        loop.run_until_complete(server.start())
    except KeyboardInterrupt:
        pass
    finally:
        loop.run_until_complete(server.stop())
        loop.close()


if __name__ == "__main__":
    main()
