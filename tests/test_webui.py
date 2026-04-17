#!/usr/bin/env python3
"""Test the web UI API endpoints."""

import asyncio
import json
import sys
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from stitch.core.protocol import MsgType, RegisterMsg
from stitch.apps.server import Server
from stitch.core.network import serve, Connection
from stitch.apps.webui import start_webui


def api_get(port, path):
    return json.loads(
        urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=2).read()
    )

def api_post(port, path, data=None):
    req = urllib.request.Request(
        f"http://127.0.0.1:{port}{path}",
        data=json.dumps(data or {}).encode(),
        headers={"Content-Type": "application/json"},
    )
    return json.loads(urllib.request.urlopen(req, timeout=2).read())


async def run_test():
    TCP_PORT = 24830
    WEB_PORT = 8091
    passed = 0

    server = Server(host="127.0.0.1", port=TCP_PORT)
    server._loop = asyncio.get_running_loop()
    srv = await serve("127.0.0.1", TCP_PORT, server._handle_client)
    httpd = start_webui(server, host="127.0.0.1", port=WEB_PORT)

    try:
        # Register two mock clients
        r1, w1 = await asyncio.open_connection("127.0.0.1", TCP_PORT)
        c1 = Connection(r1, w1)
        await c1.send(MsgType.REGISTER, RegisterMsg(
            machine_id="pc1",
            monitors=[
                {"id": "HDMI-1", "local_x": 0, "local_y": 0,
                 "width": 1920, "height": 1080},
                {"id": "DP-1", "local_x": 1920, "local_y": 0,
                 "width": 2560, "height": 1440},
            ],
        ))
        r2, w2 = await asyncio.open_connection("127.0.0.1", TCP_PORT)
        c2 = Connection(r2, w2)
        await c2.send(MsgType.REGISTER, RegisterMsg(
            machine_id="pc2",
            monitors=[
                {"id": "HDMI-1", "local_x": 0, "local_y": 0,
                 "width": 3840, "height": 2160},
            ],
        ))
        await asyncio.sleep(0.5)

        # 1: GET /api/layout returns all displays with numbering
        print("Test 1: GET /api/layout")
        data = api_get(WEB_PORT, "/api/layout")
        assert len(data["displays"]) == 3
        assert "pc1" in data["machines"]
        assert "pc2" in data["machines"]
        numbers = sorted(d["number"] for d in data["displays"])
        assert numbers == [1, 2, 3]
        print(f"  [PASS] 3 displays, 2 machines, numbered {numbers}")
        passed += 1

        # 2: Display details are correct
        print("Test 2: Display details")
        pc2_disp = [d for d in data["displays"] if d["machine_id"] == "pc2"]
        assert len(pc2_disp) == 1
        assert pc2_disp[0]["width"] == 3840
        assert pc2_disp[0]["height"] == 2160
        print(f"  [PASS] PC2 display: {pc2_disp[0]['width']}x{pc2_disp[0]['height']}")
        passed += 1

        # 3: POST /api/layout repositions displays
        print("Test 3: POST /api/layout")
        api_post(WEB_PORT, "/api/layout", {"displays": [
            {"machine_id": "pc1", "monitor_id": "HDMI-1",
             "global_x": 0, "global_y": 1440},
            {"machine_id": "pc1", "monitor_id": "DP-1",
             "global_x": 0, "global_y": 0},
            {"machine_id": "pc2", "monitor_id": "HDMI-1",
             "global_x": 2560, "global_y": 0},
        ]})
        data2 = api_get(WEB_PORT, "/api/layout")
        pos = {d["monitor_id"]: (d["global_x"], d["global_y"])
               for d in data2["displays"]}
        assert pos["HDMI-1"] == (0, 1440) or pos.get("HDMI-1") == (2560, 0)
        # Find PC1's HDMI-1
        pc1_hdmi = [d for d in data2["displays"]
                    if d["machine_id"] == "pc1" and d["monitor_id"] == "HDMI-1"][0]
        assert pc1_hdmi["global_y"] == 1440
        print(f"  [PASS] PC1:HDMI-1 moved to y=1440 (below DP-1)")
        passed += 1

        # 4: POST /api/identify succeeds
        print("Test 4: POST /api/identify")
        result = api_post(WEB_PORT, "/api/identify")
        assert result["ok"] is True
        print("  [PASS] Identify triggered successfully")
        passed += 1

        # 5: GET / serves web UI HTML
        print("Test 5: Web UI HTML")
        html = urllib.request.urlopen(
            f"http://127.0.0.1:{WEB_PORT}/", timeout=2
        ).read().decode()
        assert "Stitch" in html
        assert "canvas" in html
        assert "doIdentify" in html
        assert "doApply" in html
        print("  [PASS] HTML contains UI components")
        passed += 1

        await c1.close()
        await c2.close()

    finally:
        httpd.shutdown()
        srv.close()

    print(f"\n{passed}/5 web UI tests passed!")
    return passed == 5


if __name__ == "__main__":
    ok = asyncio.run(run_test())
    sys.exit(0 if ok else 1)
