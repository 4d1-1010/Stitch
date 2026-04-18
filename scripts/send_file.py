#!/usr/bin/env python3
"""
Send a file to another machine in the UnIO network.

Usage:
    python send_file.py --server HOST --from MACHINE_ID --to MACHINE_ID --file PATH
"""

import argparse
import asyncio
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from unio.core.network import connect
from unio.core.protocol import MsgType, RegisterMsg, RegisterAckMsg
from unio.features.filetransfer import FileTransferSender


async def send(args):
    conn = await connect(args.server, args.port)

    # Register as the sender machine (minimal, just for routing)
    await conn.send(MsgType.REGISTER, RegisterMsg(
        machine_id=args.sender,
        monitors=[{"id": "virtual", "local_x": 0, "local_y": 0,
                    "width": 1, "height": 1}],
    ))

    # Wait for ACK
    result = await conn.recv()
    if result is None:
        print("Connection lost.")
        return

    sender = FileTransferSender(conn, args.target)
    ok = await sender.send_file(args.file)
    if ok:
        print(f"File sent successfully to {args.target}")
    else:
        print("File transfer failed.")

    await conn.close()


def main():
    parser = argparse.ArgumentParser(description="Send file via UnIO")
    parser.add_argument("--server", required=True, help="Server host")
    parser.add_argument("--port", type=int, default=24800)
    parser.add_argument("--from", dest="sender", required=True, help="Source machine ID")
    parser.add_argument("--to", dest="target", required=True, help="Target machine ID")
    parser.add_argument("--file", required=True, help="File to send")
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s [%(levelname)s] %(message)s")
    asyncio.run(send(args))


if __name__ == "__main__":
    main()
