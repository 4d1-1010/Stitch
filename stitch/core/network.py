"""
Async TCP networking layer.

Provides Connection (wraps asyncio streams), plus server accept loop
and client connect helpers.
"""

import asyncio
import logging
from typing import Optional, Callable, Awaitable

from .protocol import (
    MsgType, HEADER_SIZE, encode_message, decode_header, decode_payload,
)

log = logging.getLogger(__name__)


class Connection:
    """A framed message connection over TCP."""

    def __init__(self, reader: asyncio.StreamReader,
                 writer: asyncio.StreamWriter, label: str = ""):
        self.reader = reader
        self.writer = writer
        self.label = label
        self._closed = False

    @property
    def closed(self) -> bool:
        return self._closed

    @property
    def peername(self) -> str:
        try:
            return str(self.writer.get_extra_info("peername"))
        except Exception:
            return "unknown"

    async def send(self, msg_type: MsgType, payload=None):
        """Send a framed message."""
        if self._closed:
            return
        data = encode_message(msg_type, payload or {})
        self.writer.write(data)
        await self.writer.drain()

    async def recv(self) -> Optional[tuple[MsgType, object]]:
        """
        Receive one framed message.

        Returns (msg_type, decoded_payload) or None on EOF.
        """
        try:
            header = await self.reader.readexactly(HEADER_SIZE)
        except (asyncio.IncompleteReadError, ConnectionError, OSError):
            self._closed = True
            return None

        msg_type, length = decode_header(header)

        if length > 0:
            try:
                body = await self.reader.readexactly(length)
            except (asyncio.IncompleteReadError, ConnectionError, OSError):
                self._closed = True
                return None
        else:
            body = b""

        payload = decode_payload(msg_type, body)
        return (msg_type, payload)

    async def close(self):
        if not self._closed:
            self._closed = True
            try:
                self.writer.close()
                await self.writer.wait_closed()
            except Exception:
                pass


# ── Helpers ──────────────────────────────────────────────────────────

async def connect(host: str, port: int, label: str = "") -> Connection:
    """Open a client connection to a server."""
    reader, writer = await asyncio.open_connection(host, port)
    conn = Connection(reader, writer, label=label or f"{host}:{port}")
    log.info("Connected to %s:%d", host, port)
    return conn


async def serve(
    host: str,
    port: int,
    on_connect: Callable[[Connection], Awaitable[None]],
) -> asyncio.Server:
    """
    Start a TCP server that calls on_connect for each new connection.

    Returns the asyncio.Server (call .close() to stop).
    """

    async def _handle(reader: asyncio.StreamReader,
                      writer: asyncio.StreamWriter):
        peername = writer.get_extra_info("peername")
        conn = Connection(reader, writer, label=str(peername))
        log.info("Accepted connection from %s", peername)
        try:
            await on_connect(conn)
        except Exception:
            log.exception("Error handling connection %s", peername)
        finally:
            await conn.close()

    server = await asyncio.start_server(_handle, host, port)
    addrs = [str(s.getsockname()) for s in server.sockets]
    log.info("Server listening on %s", ", ".join(addrs))
    return server
