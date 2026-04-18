#!/usr/bin/env python3
"""Integration test: server ↔ client registration and handoff over TCP."""

import asyncio
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from unio.core.protocol import (
    MsgType, RegisterMsg, EdgeHitMsg, MouseMoveRelMsg,
    ClipboardUpdateMsg, HEADER_SIZE,
    encode_message, decode_header, decode_payload,
)
from unio.core.network import Connection


async def raw_connect(port):
    reader, writer = await asyncio.open_connection("127.0.0.1", port)
    return Connection(reader, writer)


async def collect(conn, timeout=0.5, max_msgs=10):
    msgs = []
    for _ in range(max_msgs):
        try:
            result = await asyncio.wait_for(conn.recv(), timeout=timeout)
            if result is None:
                break
            msgs.append(result)
        except asyncio.TimeoutError:
            break
    return msgs


async def run_test():
    PORT = 24810

    # Import and create server
    from unio.apps.server import Server
    from unio.core.network import serve

    server = Server(host="127.0.0.1", port=PORT)
    srv = await serve("127.0.0.1", PORT, server._handle_client)

    passed = 0
    total = 5

    try:
        # ── Test 1: Registration ─────────────────────
        print("Test 1: Registration")
        c1 = await raw_connect(PORT)
        await c1.send(MsgType.REGISTER, RegisterMsg(
            machine_id="pc1",
            monitors=[{"id": "s0", "local_x": 0, "local_y": 0,
                        "width": 1920, "height": 1080}],
        ))
        await asyncio.sleep(0.2)
        msgs = await collect(c1)
        types = [m[0] for m in msgs]
        assert MsgType.REGISTER_ACK in types
        assert MsgType.ACTIVATE in types
        assert MsgType.LAYOUT_UPDATE in types
        print("  [PASS] PC1 registered + activated + layout")
        passed += 1

        # ── Test 2: Second client ────────────────────
        print("Test 2: Second client registration")
        c2 = await raw_connect(PORT)
        await c2.send(MsgType.REGISTER, RegisterMsg(
            machine_id="pc2",
            monitors=[{"id": "s0", "local_x": 0, "local_y": 0,
                        "width": 1920, "height": 1080}],
        ))
        await asyncio.sleep(0.2)
        msgs2 = await collect(c2)
        types2 = [m[0] for m in msgs2]
        assert MsgType.REGISTER_ACK in types2
        assert MsgType.LAYOUT_UPDATE in types2
        # Verify layout
        for mt, payload in msgs2:
            if mt == MsgType.LAYOUT_UPDATE:
                mons = payload.monitors
                assert len(mons) == 2
                break
        print("  [PASS] PC2 registered, layout has 2 monitors")
        passed += 1

        # Drain PC1's layout update
        await collect(c1, timeout=0.3)

        # ── Test 3: Edge crossing ────────────────────
        print("Test 3: Edge crossing handoff")
        await c1.send(MsgType.EDGE_HIT, EdgeHitMsg(
            edge="right", global_x=1919, global_y=540,
            machine_id="pc1",
        ))
        await asyncio.sleep(0.3)

        # PC1 should get DEACTIVATE
        msgs_d = await collect(c1, timeout=1.0)
        types_d = [m[0] for m in msgs_d]
        assert MsgType.DEACTIVATE in types_d, f"PC1 missing DEACTIVATE: {types_d}"

        # PC2 should get ACTIVATE
        msgs_a = await collect(c2, timeout=1.0)
        types_a = [m[0] for m in msgs_a]
        assert MsgType.ACTIVATE in types_a, f"PC2 missing ACTIVATE: {types_a}"
        print("  [PASS] Handoff: PC1→DEACTIVATE, PC2→ACTIVATE")
        passed += 1

        # ── Test 4: Input forwarding ─────────────────
        print("Test 4: Input forwarding")
        await c1.send(MsgType.MOUSE_MOVE_REL, MouseMoveRelMsg(dx=10, dy=-5))
        await asyncio.sleep(0.1)
        msgs_fwd = await collect(c2, timeout=0.5)
        found = False
        for mt, p in msgs_fwd:
            if mt == MsgType.MOUSE_MOVE_REL:
                dx = p.dx if hasattr(p, 'dx') else p.get('dx')
                assert dx == 10
                found = True
                break
        assert found, "PC2 didn't receive forwarded mouse"
        print("  [PASS] Mouse delta forwarded to PC2")
        passed += 1

        # ── Test 5: Clipboard sync ───────────────────
        print("Test 5: Clipboard sync")
        await c1.send(MsgType.CLIPBOARD_UPDATE, ClipboardUpdateMsg(
            content="test clipboard", source_machine="pc1",
        ))
        await asyncio.sleep(0.1)
        msgs_c = await collect(c2, timeout=0.5)
        found = False
        for mt, p in msgs_c:
            if mt == MsgType.CLIPBOARD_UPDATE:
                content = p.content if hasattr(p, 'content') else p.get('content')
                assert content == "test clipboard"
                found = True
                break
        assert found, "PC2 didn't receive clipboard"
        print("  [PASS] Clipboard synced to PC2")
        passed += 1

    except AssertionError as e:
        print(f"  [FAIL] {e}")
    except Exception as e:
        print(f"  [ERROR] {e}")
    finally:
        # Close client connections first
        for c in [c1, c2]:
            try:
                await c.close()
            except Exception:
                pass
        # Give handlers a moment to notice closed connections
        await asyncio.sleep(0.2)
        srv.close()
        # Don't wait_closed (handlers may still be blocked)

    print(f"\n{passed}/{total} tests passed!")
    return passed == total


if __name__ == "__main__":
    ok = asyncio.run(run_test())
    sys.exit(0 if ok else 1)
