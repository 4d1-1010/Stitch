#!/usr/bin/env python3
"""Drive a Linux unio-pipe helper over UDS. Prints server replies."""
import json, socket, struct, sys

def rpc(sock_path, cmd):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    b = json.dumps(cmd).encode()
    s.sendall(struct.pack("<I", len(b)) + b)
    rl = b""
    while len(rl) < 4:
        c = s.recv(4 - len(rl))
        if not c: raise RuntimeError("short read len")
        rl += c
    n = struct.unpack("<I", rl)[0]
    r = b""
    while len(r) < n:
        c = s.recv(n - len(r))
        if not c: raise RuntimeError("short read body")
        r += c
    s.close()
    return r.decode()

if __name__ == "__main__":
    path = sys.argv[1]
    args = sys.argv[2:]
    if args[0] == "caps":
        print(rpc(path, {"cmd":"helper_caps"}))
    elif args[0] == "status":
        print(rpc(path, {"cmd":"helper_status"}))
    elif args[0] == "out":
        peer, port, w, h, sid = args[1], int(args[2]), int(args[3]), int(args[4]), args[5]
        cx = int(args[6]) if len(args) > 6 else 0
        cy = int(args[7]) if len(args) > 7 else 0
        print(rpc(path, {
            "cmd":"start_outbound",
            "stream_id": sid,
            "peer_addr": peer,
            "peer_port": port,
            "width": w, "height": h, "fps": 30,
            "capture_x": cx, "capture_y": cy,
            "monitor_source": ":1",
        }))
    elif args[0] == "in":
        port, w, h, sid = int(args[1]), int(args[2]), int(args[3]), args[4]
        print(rpc(path, {
            "cmd":"start_inbound",
            "stream_id": sid,
            "listen_port": port,
            "window_w": w, "window_h": h,
        }))
    elif args[0] == "stop":
        sid = args[1]
        print(rpc(path, {"cmd":"stop","stream_id": sid}))
