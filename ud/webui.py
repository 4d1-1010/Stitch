"""
Web-based display configuration UI.

Serves an HTTP interface for:
  - Viewing all connected machines and their displays
  - Drag-and-drop display arrangement
  - Triggering display identification
  - Applying layout changes

The HTML/CSS/JS is embedded in this file to avoid static file path issues.
"""

import asyncio
import json
import logging
from http.server import HTTPServer, BaseHTTPRequestHandler
from threading import Thread
from typing import Optional, TYPE_CHECKING

if TYPE_CHECKING:
    from .server import Server

log = logging.getLogger(__name__)


# ── HTML/CSS/JS (embedded) ───────────────────────────────────────

_HTML = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Stitch - Display Configuration</title>
<style>
* { margin: 0; padding: 0; box-sizing: border-box; }
body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
       background: #1a1a2e; color: #e0e0e0; overflow: hidden; height: 100vh; }

.header { background: #16213e; padding: 12px 24px; display: flex;
           align-items: center; justify-content: space-between;
           border-bottom: 1px solid #0f3460; }
.header h1 { font-size: 18px; font-weight: 600; color: #e94560; }
.header .status { font-size: 13px; color: #888; }

.main { display: flex; height: calc(100vh - 52px); }

.sidebar { width: 260px; background: #16213e; border-right: 1px solid #0f3460;
           padding: 16px; overflow-y: auto; flex-shrink: 0; }
.sidebar h2 { font-size: 14px; text-transform: uppercase; letter-spacing: 1px;
              color: #888; margin-bottom: 12px; }
.machine-card { background: #1a1a2e; border-radius: 8px; padding: 12px;
                margin-bottom: 10px; border-left: 4px solid; }
.machine-card .name { font-weight: 600; font-size: 15px; margin-bottom: 4px; }
.machine-card .info { font-size: 12px; color: #888; }
.machine-card .monitor { font-size: 12px; color: #aaa; padding: 2px 0 2px 8px; }

.canvas-area { flex: 1; position: relative; }
canvas { width: 100%; height: 100%; display: block; cursor: default; }

.toolbar { position: absolute; bottom: 20px; left: 50%; transform: translateX(-50%);
           display: flex; gap: 10px; background: #16213e; padding: 10px 16px;
           border-radius: 10px; box-shadow: 0 4px 20px rgba(0,0,0,0.4); }
.btn { padding: 8px 20px; border: none; border-radius: 6px; font-size: 14px;
       cursor: pointer; font-weight: 500; transition: all 0.15s; }
.btn-identify { background: #e94560; color: white; }
.btn-identify:hover { background: #d63851; }
.btn-apply { background: #0f3460; color: white; }
.btn-apply:hover { background: #1a4a7a; }
.btn-reset { background: #333; color: #ccc; }
.btn-reset:hover { background: #444; }
.btn:disabled { opacity: 0.4; cursor: not-allowed; }

.toast { position: fixed; top: 70px; right: 20px; background: #0f3460;
         padding: 12px 20px; border-radius: 8px; font-size: 14px;
         transform: translateX(120%); transition: transform 0.3s; z-index: 100; }
.toast.show { transform: translateX(0); }
.toast.error { background: #e94560; }
</style>
</head>
<body>
<div class="header">
  <h1>Stitch</h1>
  <div class="status" id="status">Loading...</div>
</div>
<div class="main">
  <div class="sidebar" id="sidebar"></div>
  <div class="canvas-area">
    <canvas id="canvas"></canvas>
    <div class="toolbar">
      <button class="btn btn-identify" onclick="doIdentify()">Identify Displays</button>
      <button class="btn btn-apply" id="applyBtn" onclick="doApply()" disabled>Apply Layout</button>
      <button class="btn btn-reset" onclick="doReset()">Reset</button>
    </div>
  </div>
</div>
<div class="toast" id="toast"></div>

<script>
const MACHINE_COLORS = [
  '#e94560', '#0f3460', '#6a5acd', '#2ecc71', '#e67e22',
  '#1abc9c', '#9b59b6', '#e74c3c', '#3498db', '#f39c12'
];
const SNAP_DIST = 15;
const PADDING = 60;

let displays = [];
let originalDisplays = [];
let machines = {};
let colorMap = {};
let dragging = null;
let dragOff = { x: 0, y: 0 };
let scale = 1;
let offsetX = 0, offsetY = 0;
let dirty = false;
let canvas, ctx;

function toast(msg, err) {
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.className = 'toast show' + (err ? ' error' : '');
  setTimeout(() => t.className = 'toast', 3000);
}

async function fetchLayout() {
  try {
    const r = await fetch('/api/layout');
    const data = await r.json();
    displays = data.displays || [];
    machines = data.machines || {};
    originalDisplays = JSON.parse(JSON.stringify(displays));

    let ci = 0;
    for (const mid in machines) {
      if (!(mid in colorMap)) colorMap[mid] = MACHINE_COLORS[ci % MACHINE_COLORS.length];
      ci++;
    }
    computeView();
    renderSidebar();
    render();
    const mc = Object.keys(machines).length;
    const dc = displays.length;
    document.getElementById('status').textContent =
      `${mc} machine${mc!==1?'s':''} connected, ${dc} display${dc!==1?'s':''}`;
  } catch(e) {
    document.getElementById('status').textContent = 'Connection error';
  }
}

function renderSidebar() {
  let html = '<h2>Machines</h2>';
  for (const [mid, info] of Object.entries(machines)) {
    const c = colorMap[mid] || '#888';
    const mons = displays.filter(d => d.machine_id === mid);
    html += `<div class="machine-card" style="border-color:${c}">`;
    html += `<div class="name" style="color:${c}">${mid}</div>`;
    html += `<div class="info">${mons.length} display${mons.length!==1?'s':''}</div>`;
    for (const m of mons) {
      html += `<div class="monitor">#${m.number} ${m.monitor_id} - ${m.width}x${m.height}</div>`;
    }
    html += '</div>';
  }
  document.getElementById('sidebar').innerHTML = html;
}

function computeView() {
  if (!displays.length) { scale = 1; offsetX = 0; offsetY = 0; return; }
  const cw = canvas.width, ch = canvas.height;
  let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
  for (const d of displays) {
    minX = Math.min(minX, d.global_x);
    minY = Math.min(minY, d.global_y);
    maxX = Math.max(maxX, d.global_x + d.width);
    maxY = Math.max(maxY, d.global_y + d.height);
  }
  const bw = maxX - minX, bh = maxY - minY;
  scale = Math.min((cw - PADDING*2) / Math.max(bw,1), (ch - PADDING*2) / Math.max(bh,1));
  scale = Math.min(scale, 0.35);
  offsetX = (cw - bw * scale) / 2 - minX * scale;
  offsetY = (ch - bh * scale) / 2 - minY * scale;
}

function toScreen(gx, gy) {
  return { x: gx * scale + offsetX, y: gy * scale + offsetY };
}
function toGlobal(sx, sy) {
  return { x: (sx - offsetX) / scale, y: (sy - offsetY) / scale };
}

function render() {
  if (!ctx) return;
  const cw = canvas.width, ch = canvas.height;
  ctx.clearRect(0, 0, cw, ch);

  // Grid
  ctx.strokeStyle = '#222244';
  ctx.lineWidth = 1;
  const step = 100 * scale;
  if (step > 5) {
    const g0 = toGlobal(0, 0);
    const startGx = Math.floor(g0.x / 100) * 100;
    const startGy = Math.floor(g0.y / 100) * 100;
    for (let gx = startGx; ; gx += 100) {
      const sx = gx * scale + offsetX;
      if (sx > cw) break;
      if (sx < 0) continue;
      ctx.beginPath(); ctx.moveTo(sx, 0); ctx.lineTo(sx, ch); ctx.stroke();
    }
    for (let gy = startGy; ; gy += 100) {
      const sy = gy * scale + offsetY;
      if (sy > ch) break;
      if (sy < 0) continue;
      ctx.beginPath(); ctx.moveTo(0, sy); ctx.lineTo(cw, sy); ctx.stroke();
    }
  }

  // Displays
  for (const d of displays) {
    const s = toScreen(d.global_x, d.global_y);
    const w = d.width * scale, h = d.height * scale;
    const c = colorMap[d.machine_id] || '#888';
    const isDragging = dragging === d;

    // Shadow
    ctx.shadowColor = isDragging ? c : 'rgba(0,0,0,0.3)';
    ctx.shadowBlur = isDragging ? 20 : 8;
    ctx.shadowOffsetX = 0; ctx.shadowOffsetY = isDragging ? 4 : 2;

    // Fill
    ctx.fillStyle = isDragging ? c + '40' : c + '25';
    ctx.fillRect(s.x, s.y, w, h);
    ctx.shadowColor = 'transparent';

    // Border
    ctx.strokeStyle = c;
    ctx.lineWidth = isDragging ? 3 : 2;
    ctx.strokeRect(s.x, s.y, w, h);

    // Number (large, centered)
    const numSize = Math.max(14, Math.min(72, h * 0.35));
    ctx.fillStyle = '#ffffff';
    ctx.font = `bold ${numSize}px Helvetica, sans-serif`;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillText(String(d.number), s.x + w/2, s.y + h/2 - numSize*0.2);

    // Label
    const lblSize = Math.max(9, Math.min(18, h * 0.09));
    ctx.fillStyle = '#aaaaaa';
    ctx.font = `${lblSize}px Helvetica, sans-serif`;
    ctx.fillText(`${d.machine_id}`, s.x + w/2, s.y + h/2 + numSize*0.35);

    // Resolution (bottom)
    const resSize = Math.max(8, Math.min(14, h * 0.07));
    ctx.fillStyle = '#666666';
    ctx.font = `${resSize}px Helvetica, sans-serif`;
    ctx.fillText(`${d.width}x${d.height}`, s.x + w/2, s.y + h - resSize - 4);

    // Monitor ID (top-left)
    ctx.fillStyle = c + 'aa';
    ctx.font = `${resSize}px Helvetica, sans-serif`;
    ctx.textAlign = 'left';
    ctx.fillText(d.monitor_id, s.x + 6, s.y + resSize + 4);
  }
  ctx.textAlign = 'start';
}

function hitTest(sx, sy) {
  for (let i = displays.length - 1; i >= 0; i--) {
    const d = displays[i];
    const s = toScreen(d.global_x, d.global_y);
    const w = d.width * scale, h = d.height * scale;
    if (sx >= s.x && sx <= s.x + w && sy >= s.y && sy <= s.y + h) return d;
  }
  return null;
}

function snapDisplay(d) {
  const threshold = SNAP_DIST / scale;
  let bestDx = Infinity, bestDy = Infinity, snapX = d.global_x, snapY = d.global_y;
  const dR = d.global_x + d.width, dB = d.global_y + d.height;

  for (const o of displays) {
    if (o === d) continue;
    const oR = o.global_x + o.width, oB = o.global_y + o.height;
    // Vertical overlap check for horizontal snapping
    const vOverlap = d.global_y < oB && dB > o.global_y;
    // Horizontal overlap check for vertical snapping
    const hOverlap = d.global_x < oR && dR > o.global_x;

    // Right edge of d → left edge of o
    if (vOverlap && Math.abs(dR - o.global_x) < Math.abs(bestDx))
      { bestDx = dR - o.global_x; snapX = o.global_x - d.width; }
    // Left edge of d → right edge of o
    if (vOverlap && Math.abs(d.global_x - oR) < Math.abs(bestDx))
      { bestDx = d.global_x - oR; snapX = oR; }
    // Bottom of d → top of o
    if (hOverlap && Math.abs(dB - o.global_y) < Math.abs(bestDy))
      { bestDy = dB - o.global_y; snapY = o.global_y - d.height; }
    // Top of d → bottom of o
    if (hOverlap && Math.abs(d.global_y - oB) < Math.abs(bestDy))
      { bestDy = d.global_y - oB; snapY = oB; }

    // Align tops
    if (Math.abs(d.global_y - o.global_y) < threshold && Math.abs(d.global_y - o.global_y) < Math.abs(bestDy))
      { bestDy = d.global_y - o.global_y; snapY = o.global_y; }
    // Align bottoms
    if (Math.abs(dB - oB) < threshold && Math.abs(dB - oB) < Math.abs(bestDy))
      { bestDy = dB - oB; snapY = oB - d.height; }
    // Align lefts
    if (Math.abs(d.global_x - o.global_x) < threshold && Math.abs(d.global_x - o.global_x) < Math.abs(bestDx))
      { bestDx = d.global_x - o.global_x; snapX = o.global_x; }
    // Align rights
    if (Math.abs(dR - oR) < threshold && Math.abs(dR - oR) < Math.abs(bestDx))
      { bestDx = dR - oR; snapX = oR - d.width; }
  }

  if (Math.abs(bestDx) < threshold) d.global_x = Math.round(snapX);
  if (Math.abs(bestDy) < threshold) d.global_y = Math.round(snapY);
}

// Mouse handling
function onMouseDown(e) {
  const rect = canvas.getBoundingClientRect();
  const sx = (e.clientX - rect.left) * (canvas.width / rect.width);
  const sy = (e.clientY - rect.top) * (canvas.height / rect.height);
  const hit = hitTest(sx, sy);
  if (hit) {
    dragging = hit;
    const s = toScreen(hit.global_x, hit.global_y);
    dragOff = { x: sx - s.x, y: sy - s.y };
    canvas.style.cursor = 'grabbing';
    render();
  }
}
function onMouseMove(e) {
  const rect = canvas.getBoundingClientRect();
  const sx = (e.clientX - rect.left) * (canvas.width / rect.width);
  const sy = (e.clientY - rect.top) * (canvas.height / rect.height);
  if (dragging) {
    const g = toGlobal(sx - dragOff.x, sy - dragOff.y);
    dragging.global_x = Math.round(g.x);
    dragging.global_y = Math.round(g.y);
    snapDisplay(dragging);
    dirty = true;
    document.getElementById('applyBtn').disabled = false;
    render();
  } else {
    const hit = hitTest(sx, sy);
    canvas.style.cursor = hit ? 'grab' : 'default';
  }
}
function onMouseUp() {
  if (dragging) {
    dragging = null;
    canvas.style.cursor = 'default';
    render();
  }
}

async function doIdentify() {
  try {
    const r = await fetch('/api/identify', { method: 'POST' });
    if (r.ok) toast('Identification sent to all displays');
    else toast('Failed to send identification', true);
  } catch(e) { toast('Connection error', true); }
}

async function doApply() {
  const layout = displays.map(d => ({
    machine_id: d.machine_id,
    monitor_id: d.monitor_id,
    global_x: d.global_x,
    global_y: d.global_y,
  }));
  try {
    const r = await fetch('/api/layout', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ displays: layout }),
    });
    if (r.ok) {
      toast('Layout applied');
      dirty = false;
      document.getElementById('applyBtn').disabled = true;
      originalDisplays = JSON.parse(JSON.stringify(displays));
    } else toast('Failed to apply layout', true);
  } catch(e) { toast('Connection error', true); }
}

function doReset() {
  displays = JSON.parse(JSON.stringify(originalDisplays));
  dirty = false;
  document.getElementById('applyBtn').disabled = true;
  computeView();
  render();
  toast('Layout reset');
}

function init() {
  canvas = document.getElementById('canvas');
  ctx = canvas.getContext('2d');
  function resize() {
    canvas.width = canvas.clientWidth * devicePixelRatio;
    canvas.height = canvas.clientHeight * devicePixelRatio;
    ctx.scale(devicePixelRatio, devicePixelRatio);
    canvas.width = canvas.clientWidth;
    canvas.height = canvas.clientHeight;
    computeView();
    render();
  }
  window.addEventListener('resize', resize);
  canvas.addEventListener('mousedown', onMouseDown);
  canvas.addEventListener('mousemove', onMouseMove);
  canvas.addEventListener('mouseup', onMouseUp);
  canvas.addEventListener('mouseleave', onMouseUp);
  resize();
  fetchLayout();
  setInterval(fetchLayout, 3000);
}
document.addEventListener('DOMContentLoaded', init);
</script>
</body>
</html>"""


# ── HTTP Server ──────────────────────────────────────────────────

class _Handler(BaseHTTPRequestHandler):
    """HTTP request handler. Accesses the Server instance via server.ud_server."""

    def log_message(self, format, *args):
        log.debug("HTTP: %s", format % args)

    @property
    def ud(self) -> "Server":
        return self.server.ud_server

    def _json_response(self, data: dict, status: int = 200):
        body = json.dumps(data).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def _read_json(self) -> Optional[dict]:
        length = int(self.headers.get("Content-Length", 0))
        if length == 0:
            return {}
        body = self.rfile.read(length)
        return json.loads(body)

    def do_GET(self):
        if self.path == "/" or self.path == "/index.html":
            body = _HTML.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        elif self.path == "/api/layout":
            self._handle_get_layout()

        else:
            self.send_error(404)

    def do_POST(self):
        if self.path == "/api/layout":
            self._handle_post_layout()
        elif self.path == "/api/identify":
            self._handle_identify()
        else:
            self.send_error(404)

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    # ── API handlers ─────────────────────────────────────────────

    def _handle_get_layout(self):
        """Return current layout with all machines and their displays."""
        displays = []
        number = 1

        # Sort monitors: left to right, top to bottom
        sorted_monitors = sorted(
            self.ud.layout.monitors,
            key=lambda m: (m.global_y, m.global_x),
        )

        for m in sorted_monitors:
            displays.append({
                "machine_id": m.machine_id,
                "monitor_id": m.monitor_id,
                "global_x": m.global_x,
                "global_y": m.global_y,
                "width": m.width,
                "height": m.height,
                "number": number,
            })
            number += 1

        machines = {}
        for cs in self.ud.clients.values():
            machines[cs.machine_id] = {
                "monitor_count": len(cs.monitors),
                "monitors": cs.monitors,
            }

        self._json_response({
            "displays": displays,
            "machines": machines,
            "active_machine": self.ud.active_machine or "",
        })

    def _handle_post_layout(self):
        """Update display positions from the web UI."""
        data = self._read_json()
        if not data or "displays" not in data:
            self._json_response({"error": "missing displays"}, 400)
            return

        new_positions = data["displays"]

        # Update monitor positions in the layout manager
        for pos in new_positions:
            mid = pos["machine_id"]
            mon_id = pos["monitor_id"]
            gx = pos["global_x"]
            gy = pos["global_y"]
            for m in self.ud.layout.monitors:
                if m.machine_id == mid and m.monitor_id == mon_id:
                    m.global_x = gx
                    m.global_y = gy
                    break

        # Recompute machine origins
        by_machine: dict[str, list] = {}
        for m in self.ud.layout.monitors:
            by_machine.setdefault(m.machine_id, []).append(m)
        for mid, mons in by_machine.items():
            min_gx = min(m.global_x for m in mons)
            min_gy = min(m.global_y for m in mons)
            self.ud.layout._machine_origin[mid] = (min_gx, min_gy)

        self.ud.layout._rebuild_adjacency()

        # Broadcast updated layout to all clients
        asyncio.run_coroutine_threadsafe(
            self.ud._broadcast_layout(), self.ud._loop,
        )

        # Save to layout.json
        _save_layout(self.ud.layout)

        self._json_response({"ok": True})

    def _handle_identify(self):
        """Trigger display identification on all machines."""
        asyncio.run_coroutine_threadsafe(
            self.ud._trigger_identify(), self.ud._loop,
        )
        self._json_response({"ok": True})


def _save_layout(layout):
    """Save current layout to layout.json."""
    data = {"displays": []}
    for m in layout.monitors:
        data["displays"].append({
            "machine_id": m.machine_id,
            "monitor_id": m.monitor_id,
            "global_x": m.global_x,
            "global_y": m.global_y,
            "width": m.width,
            "height": m.height,
        })
    try:
        with open("layout.json", "w") as f:
            json.dump(data, f, indent=2)
        log.info("Layout saved to layout.json")
    except OSError as e:
        log.warning("Failed to save layout: %s", e)


def load_saved_layout() -> Optional[list[dict]]:
    """Load layout from layout.json if it exists."""
    try:
        with open("layout.json") as f:
            data = json.load(f)
        return data.get("displays", [])
    except (OSError, json.JSONDecodeError):
        return None


# ── Start / Stop ─────────────────────────────────────────────────

def start_webui(server: "Server", host: str = "0.0.0.0",
                port: int = 8080) -> HTTPServer:
    """Start the web UI HTTP server in a background thread."""
    httpd = HTTPServer((host, port), _Handler)
    httpd.ud_server = server  # type: ignore
    thread = Thread(target=httpd.serve_forever, daemon=True, name="webui")
    thread.start()
    log.info("Web UI available at http://%s:%d",
             "localhost" if host == "0.0.0.0" else host, port)
    return httpd
