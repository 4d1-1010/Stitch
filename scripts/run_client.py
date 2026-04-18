#!/usr/bin/env python3
"""
UnIO — Client entry point.

Usage:
    python run_client.py --id MACHINE_ID --server HOST [--port PORT]
"""

import argparse
import asyncio
import logging
import signal
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from unio.apps.client import Client


def main():
    parser = argparse.ArgumentParser(description="UnIO Client")
    parser.add_argument("--id", required=True, help="Machine ID (unique name)")
    parser.add_argument("--server", required=True, help="Server hostname or IP")
    parser.add_argument("--port", type=int, default=24800, help="Server port")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    )

    client = Client(
        machine_id=args.id,
        server_host=args.server,
        server_port=args.port,
    )

    loop = asyncio.new_event_loop()

    def shutdown():
        loop.create_task(client.stop())
        loop.call_later(1, loop.stop)

    if sys.platform != "win32":
        for sig in (signal.SIGINT, signal.SIGTERM):
            loop.add_signal_handler(sig, shutdown)

    try:
        loop.run_until_complete(client.run())
    except KeyboardInterrupt:
        pass
    finally:
        loop.run_until_complete(client.stop())
        loop.close()


if __name__ == "__main__":
    main()
