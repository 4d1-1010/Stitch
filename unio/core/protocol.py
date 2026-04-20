"""
Wire protocol for unIO.

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
    LAYOUT_APPLY = 0x04          # configurator → server: push new positions
    APPLY_MONITORS = 0x05        # server → client: reconfigure OS displays

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
    CLAIM_FOCUS = 0x24           # client → server: my mouse moved, make me active
    SET_INPUT_SOURCE = 0x25      # deprecated — the "make source" feature is gone
    INPUT_SOURCE_STATE = 0x26    # deprecated — kept for old clients only
    SET_INPUT_MUTED = 0x27       # configurator → server: ignore a PC's keyboard+mouse
    SET_CLIPBOARD_SYNC = 0x28    # configurator → server: whether a PC syncs clipboard

    # Full-mesh P2P control plane
    HELLO = 0x60                 # peer ↔ peer: identify + presence at connect
    STATE_SYNC = 0x61            # peer → newcomer: full LWW dump
    SET_STATE = 0x62             # peer → peers: LWW register update (gossip)
    CURSOR_RELEASE = 0x63        # peer → peer: hand cursor off directly
    SWAP_INPUT = 0x64            # peer → peer: passthrough click/scroll on a
                                 # swap-sink display, target = source monitor

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
    REQUEST_IDENTIFY = 0x52      # configurator → server: trigger IDENTIFY on all clients

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
    monitors: list
    os: str = ""             # platform.system() (e.g. "Linux", "Windows", "Darwin")
    platform_info: str = ""  # free-form release/distro (e.g. "Ubuntu 24.04")


@dataclass
class RegisterAckMsg:
    ok: bool
    message: str = ""
    server_hostname: str = ""


@dataclass
class LayoutUpdateMsg:
    """Sent to all clients when layout changes."""
    monitors: list
    active_machine: str
    machines: dict[str, dict[str, str]] = field(default_factory=dict)
    input_source: str = ""


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


@dataclass
class LayoutApplyMsg:
    """Configurator tells server to apply new display positions."""
    displays: list             # [{machine_id, monitor_id, global_x, global_y}]


@dataclass
class ApplyMonitorsMsg:
    """Server tells a client to reconfigure its OS display arrangement."""
    positions: dict            # {monitor_id: [local_x, local_y]}


@dataclass
class ClaimFocusMsg:
    """A client claims the focus (its local mouse moved while dormant)."""
    machine_id: str


@dataclass
class SetInputSourceMsg:
    """Configurator tells the server which machine owns the physical KB/mouse."""
    machine_id: str


@dataclass
class InputSourceStateMsg:
    """Server tells a client whether it is the input source."""
    is_input_source: bool


@dataclass
class RequestIdentifyMsg:
    """Configurator asks the server to trigger IDENTIFY on all clients."""
    pass


@dataclass
class SetInputMutedMsg:
    """Toggle whether a PC's keyboard + mouse drive the shared cursor.
    muted=True: server drops the machine's input events. muted=False:
    accept them again. Default is unmuted for every connected PC."""
    machine_id: str
    muted: bool


@dataclass
class SetClipboardSyncMsg:
    """Toggle whether a PC participates in clipboard sync. enabled=True
    (default): copies on this PC forward to peers and incoming pastes
    write to this PC's clipboard. enabled=False: neither direction."""
    machine_id: str
    enabled: bool


# ── Full-mesh P2P control ────────────────────────────────────────────

@dataclass
class HelloMsg:
    """Sent immediately on a new peer-to-peer TCP connection. Both
    sides send one — each side learns the other's identity + its own
    monitors. `os` / `platform_info` match what the legacy RegisterMsg
    carried. `initiator` is True on the side that dialed out, so the
    receiver knows it's the one that should push STATE_SYNC."""
    machine_id: str
    hostname: str
    os: str
    platform_info: str
    monitors: list
    initiator: bool


@dataclass
class StateSyncMsg:
    """Full LWW store dump, sent from an established peer to a
    newcomer right after HELLO so the newcomer starts with the rest
    of the mesh's consensus view. `entries` is a dict keyed by state
    name, value is (ts_clock, ts_machine, serializable_value)."""
    entries: dict


@dataclass
class SetStateMsg:
    """Single LWW register update, gossiped to every connected peer
    whenever a value changes. `ts_clock` + `ts_machine` form the
    Lamport-style timestamp that arbitrates concurrent writes."""
    key: str
    ts_clock: int
    ts_machine: str
    value: object   # JSON-serializable


@dataclass
class SwapInputMsg:
    """Passthrough click / scroll for a swap-sink overlay.

    When the cursor lands on a display whose overlay shows another
    PC's content (e.g. Windows's W1 showing Linux's L4), clicks on
    the SINK PC are meaningless — they hit whatever desktop app is
    underneath the overlay instead of the displayed content. This
    message forwards the click to the SOURCE PC so it lands on the
    real target.

    ``target_monitor_id`` is the source monitor on the receiver.
    ``local_x`` / ``local_y`` are the cursor position within that
    monitor's rectangle. ``button`` follows X11 numbering (1/2/3
    for buttons; 0 = scroll-only). ``scroll_dx`` / ``scroll_dy``
    are non-zero only for scroll events."""
    from_machine: str
    target_monitor_id: str
    local_x: int
    local_y: int
    button: int = 0
    pressed: bool = False
    scroll_dx: int = 0
    scroll_dy: int = 0


@dataclass
class CursorReleaseMsg:
    """Peer-to-peer direct handoff. The current owner sends this to
    whichever peer it's passing the cursor to, carrying the entry
    point in the target's local monitor coords. The target then
    gossips a SET_STATE(active=self) so the rest of the mesh learns
    where the cursor went.

    ``target_monitor_id`` is the monitor on the receiver the entry
    coords apply to. Empty string means "pick any" (old clients that
    only have one monitor). New clients set it when the handoff
    originates from a routed-sink crossing, so the receiver knows
    which of its monitors corresponds to the sink on our side."""
    from_machine: str
    entry_local_x: int
    entry_local_y: int
    target_monitor_id: str = ""


# ── Serialization ────────────────────────────────────────────────────

_MSG_CLASS = {
    MsgType.REGISTER: RegisterMsg,
    MsgType.REGISTER_ACK: RegisterAckMsg,
    MsgType.LAYOUT_UPDATE: LayoutUpdateMsg,
    MsgType.LAYOUT_APPLY: LayoutApplyMsg,
    MsgType.APPLY_MONITORS: ApplyMonitorsMsg,
    MsgType.CLAIM_FOCUS: ClaimFocusMsg,
    MsgType.SET_INPUT_SOURCE: SetInputSourceMsg,
    MsgType.INPUT_SOURCE_STATE: InputSourceStateMsg,
    MsgType.REQUEST_IDENTIFY: RequestIdentifyMsg,
    MsgType.SET_INPUT_MUTED: SetInputMutedMsg,
    MsgType.SET_CLIPBOARD_SYNC: SetClipboardSyncMsg,
    MsgType.HELLO: HelloMsg,
    MsgType.STATE_SYNC: StateSyncMsg,
    MsgType.SET_STATE: SetStateMsg,
    MsgType.CURSOR_RELEASE: CursorReleaseMsg,
    MsgType.SWAP_INPUT: SwapInputMsg,
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
