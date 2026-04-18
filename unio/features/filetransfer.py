"""
Simple file transfer between machines.

Usage (CLI):
    python scripts/send_file.py --server HOST --from MACHINE --to MACHINE --file PATH

Protocol:
    1. Sender reads file, sends FILE_OFFER with filename + size
    2. Receiver sends FILE_ACCEPT
    3. Sender sends FILE_CHUNK messages (up to 64KB each)
    4. Sender sends FILE_DONE
    5. Receiver writes file to ~/UnIO-Received/
"""

import asyncio
import base64
import logging
import os
from pathlib import Path
from typing import Optional

from ..core.protocol import MsgType

log = logging.getLogger(__name__)

CHUNK_SIZE = 64 * 1024  # 64KB
RECEIVE_DIR = Path.home() / "UnIO-Received"


class FileTransferSender:
    """Sends a file through the server to a target machine."""

    def __init__(self, conn, target_machine: str):
        self._conn = conn
        self._target = target_machine

    async def send_file(self, filepath: str) -> bool:
        path = Path(filepath)
        if not path.exists():
            log.error("File not found: %s", filepath)
            return False

        size = path.stat().st_size
        filename = path.name

        log.info("Sending file '%s' (%d bytes) to %s",
                 filename, size, self._target)

        # Send offer
        await self._conn.send(MsgType.FILE_OFFER, {
            "filename": filename,
            "size": size,
            "target_machine": self._target,
        })

        # Send chunks
        sent = 0
        with open(path, "rb") as f:
            while True:
                chunk = f.read(CHUNK_SIZE)
                if not chunk:
                    break
                # Base64 encode for JSON transport
                await self._conn.send(MsgType.FILE_CHUNK, {
                    "data": base64.b64encode(chunk).decode("ascii"),
                    "offset": sent,
                    "target_machine": self._target,
                })
                sent += len(chunk)

        # Done
        await self._conn.send(MsgType.FILE_DONE, {
            "filename": filename,
            "total_size": sent,
            "target_machine": self._target,
        })

        log.info("File transfer complete: %s (%d bytes)", filename, sent)
        return True


class FileTransferReceiver:
    """Receives file chunks and writes the completed file."""

    def __init__(self):
        self._current_file: Optional[str] = None
        self._chunks: list[bytes] = []
        self._expected_size: int = 0

    def handle_offer(self, payload: dict):
        self._current_file = payload.get("filename", "unknown")
        self._expected_size = payload.get("size", 0)
        self._chunks.clear()
        log.info("Receiving file: %s (%d bytes)", self._current_file,
                 self._expected_size)

    def handle_chunk(self, payload: dict):
        data_b64 = payload.get("data", "")
        chunk = base64.b64decode(data_b64)
        self._chunks.append(chunk)

    def handle_done(self, payload: dict) -> Optional[str]:
        """Finalize transfer. Returns the saved file path."""
        if not self._current_file:
            return None

        RECEIVE_DIR.mkdir(parents=True, exist_ok=True)
        dest = RECEIVE_DIR / self._current_file

        # Avoid overwriting
        if dest.exists():
            stem = dest.stem
            suffix = dest.suffix
            i = 1
            while dest.exists():
                dest = RECEIVE_DIR / f"{stem}_{i}{suffix}"
                i += 1

        content = b"".join(self._chunks)
        dest.write_bytes(content)
        log.info("File saved: %s (%d bytes)", dest, len(content))

        self._current_file = None
        self._chunks.clear()
        return str(dest)
