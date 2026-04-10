"""
Wire protocol for Unified Desktop.

Frame format (little-endian):
    [type: uint16][length: uint32][payload: JSON bytes]

All coordinates are in the global virtual coordinate space unless noted.
"""

import enum
import json
import struct
from dataclasses import dataclass, field, asdict
from typing import Optional

HEADER_FMT = "<HI"  # uint16 type + uint32 length
HEADER_SIZE = struct.calcsize(HEADER_FMT)


class MsgType(enum.IntEnum):
    # Connection lifecycle
    REGISTER = 0x01
    REGISTER_ACK = 0x02
    LAYOUT_UPDATE = 0x03

    # Input events (forwarded from active → server → target)
    MOUSE_MOVE_ABS = 0x10
    MOUSE_MOVE_REL = 0x11
    MOUSE_BUTTON = 0x12
    MOUSE_SCROLL = 0x13
    KEY_EVENT = 0x14

    # Control
    EDGE_HIT = 0x20
    HANDOFF = 0x21
    ACTIVATE = 0x22
    DEACTIVATE = 0x23

    # Clipboard
    CLIPBOARD_UPDATE = 0x30

    # File transfer
    FILE_OFFER = 0x40
    FILE_ACCEPT = 0x41
    FILE_CHUNK = 0x42
    FILE_DONE = 0x43

    # Display management
    IDENTIFY = 0x50
    IDENTIFY_ACK = 0x51

    # Health
    HEARTBEAT = 0xF0
    HEARTBEAT_ACK = 0xF1


# ── Data classes for each message ────────────────────────────────────

@dataclass
class MonitorInfo:
    """Describes a single physical monitor."""
    id: str                  # unique within the machine, e.g. "HDMI-1"
    local_x: int             # X offset on the local X screen
    local_y: int
    width: int
    height: int
    # Filled in by layout manager:
    global_x: int = 0
    global_y: int = 0


@dataclass
class RegisterMsg:
    machine_id: str
    monitors: list           # list of MonitorInfo dicts


@dataclass
class RegisterAckMsg:
    ok: bool
    message: str = ""


@dataclass
class LayoutUpdateMsg:
    """Sent to all clients when layout changes."""
    monitors: list           # list of {machine_id, monitor_id, gx, gy, w, h}
    active_machine: str


@dataclass
class MouseMoveRelMsg:
    dx: int
    dy: int


@dataclass
class MouseMoveAbsMsg:
    x: int                   # global coords
    y: int


@dataclass
class MouseButtonMsg:
    button: int              # X11 button number (1=left, 2=mid, 3=right, 4/5=scroll)
    pressed: bool


@dataclass
class MouseScrollMsg:
    dx: int
    dy: int                  # positive = scroll up


@dataclass
class KeyEventMsg:
    keycode: int             # X11 keycode
    pressed: bool
    modifiers: int = 0       # modifier mask


@dataclass
class EdgeHitMsg:
    """Client tells server the cursor hit an edge."""
    edge: str                # "left", "right", "top", "bottom"
    global_x: int
    global_y: int
    machine_id: str


@dataclass
class HandoffMsg:
    """Server tells a client to take or release control."""
    target_machine: str
    entry_x: int             # global coords where cursor enters
    entry_y: int


@dataclass
class ActivateMsg:
    """Server tells client: you now own the cursor."""
    entry_local_x: int
    entry_local_y: int


@dataclass
class DeactivateMsg:
    """Server tells client: release the cursor, start forwarding."""
    pass


@dataclass
class ClipboardUpdateMsg:
    content: str
    source_machine: str


@dataclass
class HeartbeatMsg:
    seq: int = 0


@dataclass
class IdentifyMsg:
    """Server tells client to show identification overlays."""
    displays: list             # [{monitor_id, number, label}]
    duration: int = 3          # seconds to show overlay


# ── Serialization ────────────────────────────────────────────────────

_MSG_CLASS = {
    MsgType.REGISTER: RegisterMsg,
    MsgType.REGISTER_ACK: RegisterAckMsg,
    MsgType.LAYOUT_UPDATE: LayoutUpdateMsg,
    MsgType.MOUSE_MOVE_ABS: MouseMoveAbsMsg,
    MsgType.MOUSE_MOVE_REL: MouseMoveRelMsg,
    MsgType.MOUSE_BUTTON: MouseButtonMsg,
    MsgType.MOUSE_SCROLL: MouseScrollMsg,
    MsgType.KEY_EVENT: KeyEventMsg,
    MsgType.EDGE_HIT: EdgeHitMsg,
    MsgType.HANDOFF: HandoffMsg,
    MsgType.ACTIVATE: ActivateMsg,
    MsgType.DEACTIVATE: DeactivateMsg,
    MsgType.CLIPBOARD_UPDATE: ClipboardUpdateMsg,
    MsgType.IDENTIFY: IdentifyMsg,
    MsgType.HEARTBEAT: HeartbeatMsg,
    MsgType.HEARTBEAT_ACK: HeartbeatMsg,
}


def encode_message(msg_type: MsgType, payload_obj) -> bytes:
    """Encode a message into wire format."""
    if hasattr(payload_obj, '__dataclass_fields__'):
        payload = asdict(payload_obj)
    elif isinstance(payload_obj, dict):
        payload = payload_obj
    else:
        payload = {}
    body = json.dumps(payload, separators=(',', ':')).encode('utf-8')
    header = struct.pack(HEADER_FMT, int(msg_type), len(body))
    return header + body


def decode_header(data: bytes) -> tuple[MsgType, int]:
    """Decode header, return (msg_type, payload_length)."""
    raw_type, length = struct.unpack(HEADER_FMT, data)
    return MsgType(raw_type), length


def decode_payload(msg_type: MsgType, data: bytes):
    """Decode JSON payload into the appropriate dataclass."""
    if not data:
        cls = _MSG_CLASS.get(msg_type)
        return cls() if cls else {}
    payload = json.loads(data.decode('utf-8'))
    cls = _MSG_CLASS.get(msg_type)
    if cls is None:
        return payload
    # Handle nested MonitorInfo for RegisterMsg
    if cls is RegisterMsg and 'monitors' in payload:
        payload['monitors'] = [
            m if isinstance(m, dict) else asdict(m)
            for m in payload['monitors']
        ]
    try:
        return cls(**payload)
    except TypeError:
        return payload
