# 🏗️ UnIO — Architecture

> [!NOTE]
> **Status:** first draft, 2026-04-24. This document is the starting
> point for the architecture discussion; it is not yet locked.
> Sections flagged **`🟡 OPEN`** have pending decisions the team owns.
> Once every `🟡 OPEN` is resolved, this file becomes the canonical
> architectural reference and feeds directly into the requirements
> document.

---

## 1. 🎯 Product shape

UnIO is a **single signed binary per OS** (Windows, Linux). One install
artefact, one running process, one log, one stack trace. Written
entirely in **C++20 — every layer, including the UI**. The binary
bundles everything a user needs to run UnIO:

- ❌ No Python runtime.
- ❌ No browser engine (no Electron, no CEF, no WebView2, no Tauri).
- ❌ No JavaScript / TypeScript bundle.
- ❌ No Dart / Flutter.
- ❌ No scripting runtime of any kind.
- ❌ No separate helpers on disk, no sidecar processes.
- ✅ Native C++ all the way from the OS display / input APIs up to the pixel the user clicks on.

### Product commitments that drive the architecture

| # | Commitment | Architectural consequence |
|---|---|---|
| 1 | 🖥️ **Distributed multi-monitor desktop** — cursor walks every screen, keyboard follows cursor, any monitor can display any other machine's desktop. | Mesh of peers, shared CRDT state, distributed display-routing. |
| 2 | 🏠 **LAN-only — desk hardware management** — UnIO unifies PCs that share a desk or a room, not PCs across the internet. | No WAN, no relay, no NAT punching, no rendezvous server, no STUN/TURN. Discovery + all peer traffic assume direct LAN reachability. |
| 3 | ⚡ **Install, sign in, it works** — zero manual driver installs, zero sudo prompts outside the installer, no config-file editing. | All OS-level privilege done once at install time. Runtime is user-privilege-only. |
| 4 | 🌐 **Same UX everywhere** — Windows + Linux parity (macOS deferred). | Shared orchestrator module; per-OS code is isolated to a thin backend layer. |
| 5 | 🖼️ **Physical monitors only** — no virtual displays, no IDD, no evdi. | Display routing operates on real connectors; no kernel drivers shipped. |
| 6 | 🎬 **Hardware H.264 encoding required** — no software fallback. | Hosts without HW encoder refuse display streaming cleanly; other features still work. |

### Companion docs

- [`unio-pipe/ARCHITECTURE.md`](unio-pipe/ARCHITECTURE.md) — media pipeline details (capture / encode / transport / decode / present). Phase 3 of the port absorbs that tree into root `src/`; its content migrates into this doc as a subsection.
- [Issue #31](https://github.com/4d1-1010/Stitch/issues/31) — live coverage matrix (GPU tiers + routing modes).
- [Issue #34](https://github.com/4d1-1010/Stitch/issues/34) — C++ port umbrella + phase schedule.

---

## 2. 🧱 Three-layer model

One binary, three internal layers. They talk to each other via C++
function calls — there is **no IPC boundary** between them.

```mermaid
flowchart TB
    subgraph UI["🎨 UI layer"]
        UI_shell["shell · layout canvas · peer tiles · stream tiles · settings · log"]
    end

    subgraph ORCH["🧠 Orchestrator (policy + state + mesh)"]
        direction LR
        CAPS["📋 Capability registry<br/>(local + peers)"]
        SEL["🎯 Path selector<br/>(pure function,<br/>gates on #31 matrix)"]
        SESS["▶️ Session scheduler<br/>(start / stop / failover)"]
        PAIR["🔐 Peer / pairing manager"]
        MESH["🕸️ Mesh sync<br/>(LWW CRDT over QUIC)"]
    end

    subgraph PIPE["⚙️ unio-pipe (stateless media workers + local probe)"]
        direction LR
        PROBE["🔍 Capability probe"]
        CAP["🎥 Capture<br/>(WGC / XComp / PipeWire)"]
        ENCDEC["🎞️ Encoder / Decoder<br/>(VA-API / NVENC / NVDEC /<br/>D3D11VA / oneVPL / AMF)"]
        PRES["🖥️ Presenter<br/>(DXGI / EGL-X11 / EGL-Wayland)"]
        TRANS["📡 QUIC transport<br/>+ H.264 parser + latency SEI"]
    end

    UI -->|synchronous C++ calls<br/>+ pub-sub events| ORCH
    ORCH -->|in-process C++ calls| PIPE
    ORCH <-. QUIC mesh .-> ORCH2[" "]:::peer
    PIPE <-. QUIC frame bytes .-> PIPE2[" "]:::peer

    classDef peer fill:#2d2d2d,stroke:#888,stroke-dasharray:5 5,color:#aaa
```

### Responsibilities

| Layer | ✅ Owns | ❌ Must NOT know about |
|---|---|---|
| **`unio-pipe`** | Capture / encode / decode / present / QUIC frame bytes / local capability probe. | Peers · mesh · policy · user intent · pairing. |
| **Orchestrator** | Mesh CRDT · capability map · path selection · session lifecycle · fallback chains · pairing / trust. | GUI framework · window layout pixels. |
| **UI** | User intent · rendering · layout canvas · UI-local input. | Codec names · transport details · CRDT merge rules · matrix cells. |

### Why a three-layer split

- 🔒 **`unio-pipe` stays replaceable.** Narrow public interface means a future port to a different media stack touches one layer.
- 🧠 **Orchestrator is the policy seat.** Everything that's "think before you act" lives here.
- 🎨 **UI is decoration.** Swap GUI frameworks without touching the other layers.

---

## 3. ⚙️ `unio-pipe` layer (bottom)

Narrow, stateless-per-call, fast. Already exists as **~13.5 KLoC** in
`unio-pipe/src/` + `unio-pipe/include/`; Phase 0 moves that tree to
root `src/` + `include/` unchanged. Phase 3 drops the `ControlSocket`
+ separate-binary story.

<details>
<summary>📎 Public entry points (what the orchestrator calls)</summary>

```cpp
namespace unio::pipe {

  // One-shot capability probe. Cached once per process; rerun only
  // on explicit hardware-change signal.
  ProbeResult Probe();

  // StreamManager per active routing line. Owns the pipeline for
  // one direction of one stream (capture → encode → QUIC → peer,
  // or peer → QUIC → decode → present).
  class StreamManager {
   public:
    std::optional<std::string> StartOutbound(OutboundConfig);
    std::optional<std::string> StartInbound(InboundConfig);
    void Stop();
    void RequestIdr();
  };
  std::unique_ptr<StreamManager> MakeStreamManager();

}  // namespace unio::pipe
```

</details>

Stateless beyond the lifetime of an individual `StreamManager`. No
mesh state, no peer list, no pairing awareness — the orchestrator
hands `StreamManager` a pre-authenticated QUIC endpoint to talk to.

---

## 4. 🧠 Orchestrator layer (middle)

New module; doesn't exist as C++ yet. Takes over everything
`helper_bridge.py` + `unio/layout.py` + `unio/mesh.py` do today in
the Python tree, **plus** the new capability-map + path-selector work
from [#63](https://github.com/4d1-1010/Stitch/issues/63).

### Internal sub-modules

```mermaid
flowchart LR
    API["🎛️ Public API<br/>(→ UI)"]
    SESS["▶️ Session scheduler<br/>(start / stop / failover)"]
    SEL["🎯 Path selector<br/>(pure function)"]
    CAPS["📋 Capability registry<br/>(local + peers)"]
    PAIR["🔐 Peer / pairing manager"]
    MESH["🕸️ Mesh sync<br/>(LWW CRDT)"]
    PROBE_LOCAL["🔍 Local probe adapter<br/>(→ unio-pipe)"]

    API --> SESS
    API --> SEL
    API --> CAPS
    API --> PAIR

    SESS --> SEL
    SEL --> CAPS
    CAPS --> MESH
    CAPS --> PROBE_LOCAL
    PAIR --> MESH
    MESH -.QUIC.-> MESH_PEER[" "]:::peer

    classDef peer fill:#2d2d2d,stroke:#888,stroke-dasharray:5 5,color:#aaa
```

<details>
<summary>📎 Public API surface (what the UI calls)</summary>

```cpp
namespace unio::orchestrator {

// --- Probe / map queries ---
ProbeResult                         LocalCapabilities();
std::unordered_map<PcId, PeerRecord> CapabilityMap();

// --- Path planning (pure) ---
std::variant<RoutingChoice, RoutingRefusal>
PickRoute(PcId src, PcId dst, RoutingMode mode);

// --- Session control ---
StreamId    StartStream(PcId src, PcId dst,
                        DisplayRef src_disp, DisplayRef dst_disp,
                        RoutingMode mode);
void        StopStream(StreamId);
StreamState GetStreamState(StreamId);

// --- Events (pub-sub) ---
void Subscribe(EventKind, std::function<void(Event)>);

}  // namespace unio::orchestrator
```

Events the orchestrator emits:

- 👋 `PeerJoined`, `PeerLeft`, `PeerCapabilitiesChanged`
- 🟢 `StreamStarted`, 🔴 `StreamFailed`, 🟡 `StreamRecovered`, ⏹️ `StreamStopped`
- 🔐 `PairingRequest`, `PairingAccepted`, `PairingRejected`
- 🖱️ `InputEvent` (later — Phase 2)
- 📋 `ClipboardUpdate` (later — Phase 2)

</details>

---

## 5. 🎨 UI layer (top)

Consumes the orchestrator's public API. Every user interaction is
one of:

- 🔎 A **query** — "show me the capability map so I can draw the layout" → `CapabilityMap()` + `PickRoute()` per line-drop preview.
- ▶️ A **command** — "start this stream" → `StartStream(...)`.
- 📡 A **subscription** — "tell me when state changes" → `Subscribe(...)`.

No direct talk to `unio-pipe`, ever. No mesh / CRDT awareness beyond
what the orchestrator exposes through its API.

### Screens (rough, Phase 5 / [#40] refines)

- 🎨 **Layout canvas** — PCs as containers, displays as rectangles inside each PC, routing lines between displays.
- 👥 **Peer list / discovery** — who's online, who's paired, pairing prompts.
- 📺 **Stream tiles** — live status of each active stream (latency, throughput, codec in use, fallback tier if degraded).
- ⚙️ **Settings** — shortcuts, pairing codes, log verbosity.
- 📋 **Log / diagnostics** — latency histograms, last N errors, copy-to-clipboard for support.

> [!IMPORTANT]
> **🔒 LOCKED: UI is native C++.** No web framework, no embedded
> runtime, no scripting layer. Compiles into the same binary as
> the orchestrator and `unio-pipe`.

### UI toolkit — 🔒 LOCKED (2026-04-24): Dear ImGui

- **SPDX:** `MIT`. https://github.com/ocornut/imgui/blob/master/LICENSE.txt
- Pure C++, single-directory vendor, no external runtime, no installer story, no DLL/so to ship.
- **Immediate-mode / custom-rendered** — no native OS widgets. That matches the paper/lilac design language (already not trying to look OS-native in the Tk version) and makes the Layout canvas natural rather than fighting a widget toolkit.
- **Zero-copy video preview**: the app already owns a GPU context per OS (GL/X11, D3D11/Windows, Metal/macOS-future). ImGui composites on top of the same context, so presenter frames from `unio-pipe` can land directly as textures in the UI without a CPU round-trip.
- Works on Windows + Linux today; Mac ready via Metal backend for a future port.

#### Sub-decisions ImGui forces

| # | Decision | Status |
|---|---|---|
| 1a | **Platform backend**: raw Win32 on Windows + raw Xlib on Linux (Cocoa later on Mac). Zero third-party windowing deps. | 🔒 LOCKED (2026-04-24) |
| 1b | **Rendering backend** (per OS): GL3 everywhere / D3D11-Win + GL-Linux / Metal-Mac-later | 🟡 OPEN — see §13 |

##### 1a notes

- `imgui_impl_win32.cpp` is upstream + official — vendored as-is.
- `imgui_impl_x11.cpp` **does not exist upstream**; we write it (~300–500 LOC): `XCreateWindow`, `XNextEvent` → `ImGuiIO`, clipboard via `XSetSelectionOwner` / `SelectionNotify`, cursor shapes via `XcursorLibraryLoadCursor`, HiDPI via `Xft.dpi` or XRandR. IME via XIM (or explicitly deferred with a documented carveout).
- `imgui_impl_osx.cpp` is upstream + official — ready when macOS lands.
- Rationale: consistent with the project-wide preference for minimal build-time + runtime deps (`feedback_minimal_deps.md`) and the single-file static-link ship (§12). We've already written significant X11 code in `unio-pipe` (XComposite, the spike's tree-walk + pixmap plumbing) — not new territory.

---

## 6. 🕸️ Mesh / CRDT

Each pairing-trusted peer is a slot in the shared state. Slots are
**LWW (last-write-wins per key, signed by the owner's pairing key)**.

### Record types per peer slot

| Slot | Schema | Writer | Readers |
|---|---|---|---|
| `caps` | `ProbeResult` (with `SessionType`, `MonitorInfo[]`, `RoutingModes`) | 📝 own PC only | 👁️ everyone |
| `layout` | Layout graph (PCs, displays, routing lines) | 📝 one PC at a time per edit | 👁️ everyone |
| `workspaces` | Named workspace routing presets | 📝 one PC at a time per edit | 👁️ everyone |
| `pairing` | Pairing records + rotating keys | 📝 bilateral | 👁️ both paired peers |
| `clipboard` | Shared clipboard content + versioning | 📝 whichever PC wrote last | 👁️ everyone |
| `presence` | heartbeat timestamp | 📝 own PC only | 👁️ everyone |

### Transport

QUIC between peer orchestrators (same msquic stack `unio-pipe` already
uses). TLS 1.3 comes with QUIC; auth comes from pairing-derived keypairs.

**Discovery — 🔒 LOCKED (2026-04-24):** given the LAN-only product
commitment, peer discovery uses **mDNS** as the primary mechanism
(zero-config on any LAN, including home Wi-Fi), with **manual
invite / QR-code pairing** as the fallback for LANs where mDNS is
blocked (some corporate / segmented networks). **No rendezvous
server**, no external dependency for peers to find each other.

**Multi-homed PCs — bind everywhere LAN, nowhere public.** The
common desk-setup case has PCs with several active network
interfaces simultaneously (e.g. wired Ethernet + Wi-Fi on the same
router; or a wired NIC + a USB Wi-Fi dongle + a docking-station
Ethernet). Peers on one interface must be reachable from peers on
another interface, so UnIO advertises + listens on **every local
network interface whose primary address is in a private range**:

| Include | Exclude |
|---|---|
| IPv4 RFC 1918 — `10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16` | Public routable IPv4 |
| IPv4 link-local — `169.254.0.0/16` | Loopback (`127.0.0.0/8`, `::1`) |
| IPv6 link-local — `fe80::/10` | Public routable IPv6 |
| IPv6 unique-local — `fc00::/7` | Docker / VM bridges whose far side reaches the internet (inferred via default-route check) |
| VPN / tunnel interfaces whose addressing is private AND whose gateway doesn't have a default route to the internet | VPN interfaces whose default route goes to the public internet (treat as WAN — out of scope) |

Multi-homed same-PC advertisements deduplicate by the PC's pairing
pubkey (not by interface) — the orchestrator collapses "PC X via
Ethernet" + "PC X via Wi-Fi" into a single peer entry and picks
the first-responding path; stores the others as fallback routes
in case one interface drops.

### Connection model — 🔒 LOCKED (2026-04-24)

**Hybrid: two kinds of QUIC connections per paired peer.**

```mermaid
flowchart LR
    subgraph A["🖥️ Paired peer A"]
        OA["🧠 Orchestrator A"]
        PA["⚙️ unio-pipe A"]
    end
    subgraph B["🖥️ Paired peer B"]
        OB["🧠 Orchestrator B"]
        PB["⚙️ unio-pipe B"]
    end

    OA <==>|"1⃣ Control connection (always on)<br/>• presence heartbeat<br/>• CRDT sync: caps · layout · workspaces · clipboard<br/>• RPC: StartStream · StopStream · RequestIdr<br/>• pairing handshakes"| OB

    PA -. "2⃣ Media connection (per-session)<br/>• frame bytes + latency SEI only<br/>• opened on StartStream, torn down on stop<br/>• separate TLS + congestion window<br/>• orchestrator negotiates over 1⃣" .-> PB
```

#### 1⃣ Control connection — always on per paired peer

- Established once, at pairing time; stays up for the lifetime of the pairing.
- One QUIC connection per paired-peer pair.
- Multiple QUIC streams within it for independent flow control:
  - `caps` sync stream
  - `layout` sync stream
  - `workspaces` sync stream
  - `clipboard` sync stream
  - `rpc` request/response stream
  - `presence` heartbeat stream
- Low-bandwidth (KB/s at most in normal operation).
- Survives across media sessions, reconnects transparently on brief network blips, doesn't depend on any stream being active.

#### 2⃣ Media connection — per-session, dedicated to one stream

- One QUIC connection **per `StreamId`** (per active routing line).
- Opens when the orchestrator's session scheduler decides a route is valid + the remote orchestrator confirms it over the control connection.
- Teardown is explicit: `StopStream` sent on the control connection → both sides tear down the media connection.
- Carries only frame bytes + the latency SEI in-band. No control traffic, no CRDT, no heartbeats.
- Independent TLS + congestion window — keyframe bursts can't starve control heartbeats, CRDT bulk-sync on the control connection can't delay a frame.
- Reuses pairing keys for TLS; no separate pairing dance per session.

#### Orchestrator mediates the setup

The orchestrator is the only thing that opens media connections:

1. `UI → Orchestrator A : StartStream(A → B, mode)`
2. `Orchestrator A`: runs `PickRoute`, picks primary + fallback chain.
3. `Orchestrator A → Orchestrator B` over the **control connection**: RPC `prepare-media(stream_id, codec, ...)`.
4. `Orchestrator B → unio-pipe B`: `StreamManager.StartInbound(...)`; reply on control with listen address + port.
5. `Orchestrator A → unio-pipe A`: `StreamManager.StartOutbound(...)`, passing B's listen address.
6. `unio-pipe A` opens the **media connection** to `unio-pipe B`; TLS via pairing-derived cert; frames flow.
7. On stop / failure: `Orchestrator A → Orchestrator B` RPC `stop-media(stream_id)` over control; both sides tear down.

`unio-pipe` never opens a media connection without the orchestrator telling it exactly where and for what session. No direct peer discovery inside `unio-pipe`; it only sees QUIC endpoints the orchestrator hands it.

#### Rationale

- ✅ **Fault isolation.** Media stream crash → control still up, other features (layout edits, clipboard) unaffected. Control connection blip → active media streams keep running on their own connections.
- ✅ **Clean lifecycles.** "Paired" = control connection. "Streaming" = media connection. "Peer online" = heartbeats on control. One observable per state.
- ✅ **Congestion isolation.** Media bursts (keyframes, bandwidth spikes) can't delay heartbeats or CRDT ops; CRDT catch-up sync can't delay a frame.
- ✅ **Single trust anchor.** Both connection kinds use the same pairing-derived TLS material; no separate auth per stream.
- ❌ **Cost**: two TLS handshakes minimum per peer-pair with an active stream. On LAN this is ~10 ms round-trip each — negligible for session startup, invisible in steady state.

> [!WARNING]
> **🟡 OPEN: schema evolution + cadence.**
>
> - **Capability schema versioning.** Older peers' records may lack
>   new fields. Selector must treat missing fields as conservative
>   defaults. Need explicit schema-version byte + migration rules.
> - **Slot size ceiling.** Cap each `caps` record at ~4 KB JSON-equivalent?
> - **Broadcast cadence.** Event-driven on change + ~60 s heartbeat as insurance? Stale threshold ~120 s?

---

## 7. 🔄 Capability-map lifecycle

```mermaid
sequenceDiagram
    autonumber
    participant Pipe as ⚙️ unio-pipe
    participant Orch as 🧠 Orchestrator
    participant Mesh as 🕸️ Mesh CRDT
    participant Peer as 👥 Peer orchestrators

    Note over Pipe, Orch: 🟢 Boot / hardware change
    Pipe->>Orch: Probe() → ProbeResult
    Orch->>Orch: sign + write to "caps" slot
    Orch->>Mesh: replicate slot
    Mesh->>Peer: fan out signed record
    Peer->>Peer: verify signature + schema version
    Peer->>Peer: update local CapabilityMap
    Peer-->>Peer: emit CapabilitiesChanged event
    Peer-->>Peer: UI re-renders
```

### Probe refresh triggers

| Event | Triggered by |
|---|---|
| 🟢 Process start | Initial boot |
| 🖥️ Monitor hot-plug | DRM / RandR event (Linux); `WM_DISPLAYCHANGE` (Windows) |
| 🎮 GPU driver event | udev hotplug (Linux); `WM_WTSSESSION_CHANGE` (Windows) |
| 🔄 User-requested rescan | Settings screen "rescan" button |
| ⏱️ Periodic insurance | Every ~5 minutes |

---

## 8. 🎯 Path selection + fallback

Selector is **pure** — same inputs give same output on every PC. This is
what makes the synchronised map "synchronised" without synchronising the
derived decisions.

```mermaid
flowchart TD
    START["PickRoute(src, dst, mode)"]
    MATRIX{"#31 matrix cell<br/>src.session → dst.session @ mode"}
    REFUSE_CELL["🔴 RoutingRefusal<br/>(reason + fixes)"]
    INTERSECT["Intersect src.encoders × dst.decoders<br/>(matching codec)"]
    EMPTY{"candidates empty?"}
    REFUSE_EMPTY["🔴 RoutingRefusal<br/>(no compatible codec pair)"]
    RANK["Rank by LatencyTable[(enc, dec)]<br/>(seeded from matrix-test runs)"]
    RETURN["🟢 RoutingChoice(primary)<br/>+ fallback_chain[1..]"]

    START --> MATRIX
    MATRIX -- ❌ --> REFUSE_CELL
    MATRIX -- ✅ --> INTERSECT
    INTERSECT --> EMPTY
    EMPTY -- yes --> REFUSE_EMPTY
    EMPTY -- no --> RANK
    RANK --> RETURN
```

> [!TIP]
> **🟡 OPEN: latency-table seeding.**
> 
> The ranking table needs real numbers. Options:
> - 📊 Matrix-test runner ([#51] / [#65]) periodically re-runs + publishes a ranking file that ships with the binary.
> - 🎯 Per-install calibration pass on first boot.
> - 🎲 Hardcoded defaults seeded from measured adi-pc + Diana results.

---

## 9. ▶️ Session lifecycle

```mermaid
sequenceDiagram
    autonumber
    participant UI as 🎨 UI
    participant OA as 🧠 Orchestrator[A]
    participant OB as 🧠 Orchestrator[B]
    participant PA as ⚙️ unio-pipe[A]
    participant PB as ⚙️ unio-pipe[B]

    UI->>OA: StartStream(A→B, mode)
    OA->>OA: PickRoute()
    alt refusal
        OA-->>UI: 🔴 StreamFailed(reason, fixes)
    else choice + fallback chain
        OA->>OB: start-stream RPC over QUIC
        OB->>OB: verify choice still valid locally
        OB->>PB: MakeStreamManager.StartInbound()
        OB-->>OA: ok / error
        alt ok
            OA->>PA: MakeStreamManager.StartOutbound()
            OA-->>UI: 🟢 StreamStarted(id, choice)
            loop periodic
                PA-->>OA: stats (p50, p95, codec)
                OA-->>UI: StreamStats(id, ...)
            end
        else runtime failure
            OA->>OA: try next tier from fallback_chain
            Note over OA: silent until every tier exhausted
        end
    end
```

### Failover

Silent from the UI's perspective until every tier fails. The
orchestrator handles "NVENC session open failed, try oneVPL next" without
bothering the user.

> [!WARNING]
> **🟡 OPEN: session-state policies.**
>
> - **Reconnection policy.** QUIC drop for 500 ms → (a) hold + transparent reconnect, (b) tear down + restart, (c) give up after N seconds? Proposal: (a) with 5-second max before falling to (b) with next-tier fallback.
> - **Multi-viewer.** Two different PCs streaming in from A to B's same `src_disp` — share the outbound encode session? Requires `unio-pipe::StreamManager` fan-out output list.

---

## 10. 🧵 Threading model

| Concern | Thread |
|---|---|
| 🎨 Main event loop (UI events, orchestrator API calls) | UI / main thread |
| 🕸️ Mesh CRDT sync (network I/O, CRDT merging) | Dedicated mesh thread |
| 🔍 Capability probe | Main thread at boot; refresh on worker |
| 🎥 `unio-pipe` capture | Per-stream capture thread |
| 🎞️ `unio-pipe` encode | Per-stream encode thread |
| 🎞️ `unio-pipe` decode | Per-stream decode thread |
| 🖥️ `unio-pipe` presenter | Per-stream present thread |
| 📡 QUIC | msquic's own thread pool |

Orchestrator state is accessed from multiple threads; one shared
reader-writer lock around the capability map + session registry. Mutex
contention shouldn't be a hot path because everything changes at human
timescales (Hz, not kHz).

---

## 11. 🔐 Security / trust

### Threat model (LAN-only scope)

UnIO's LAN-only commitment narrows the threat model meaningfully:

- ❌ **NOT defending against**: mass internet scanning, WAN attackers, compromised relay servers, off-LAN network adversaries, DPI-based blocking, state-level adversaries.
- ✅ **Defending against**: other users / processes on the same LAN (untrusted roommates, guest Wi-Fi attackers, co-worker PCs in a shared office), stale pairings, same-user processes on the same host trying to snoop UnIO's state, lost / leaked pairing keys.

LAN ≠ trusted. Pairing is still pubkey-based + mutual.

### Controls

- 🤝 **Pairing is mutual + pubkey-based.** Two PCs exchange pubkeys out-of-band (PIN-on-one / enter-on-other, or QR). Every mesh CRDT record is signed with the owning PC's pairing key. Unsigned or invalid-signature records are rejected.
- 🧱 **No IPC inside the binary.** All three layers linked into one process → no pipe / socket an attacker can hit from another same-user process.
- 🔒 **QUIC TLS 1.3** for peer-to-peer. Cert material derived from pairing keys (no external CA).
- 🧰 **Installer privileges.** One-time elevation for OS-level capture + input-redirection permissions (WGC consent on Windows; `xdg-desktop-portal` grant on Linux). Never required at runtime.
- 🏠 **LAN scope enforcement.** See §6 for the full interface-selection table. Short version: UnIO binds to **every** local interface whose primary address is in a private / link-local / unique-local range — wired + wifi + USB NICs + docking Ethernet all at once — so multi-homed PCs remain reachable from peers on any of those interfaces. Interfaces whose default route reaches the public internet are skipped; paired connections are only established to addresses discovered via mDNS or entered manually; never to public IPs.

> [!CAUTION]
> **🟡 OPEN: capability records are sensitive.**
>
> A capability record advertises GPU model, driver version, monitor topology. Trusted peers = fine. But if pairing is broken / key leaks, an attacker learns fingerprintable info. Record schema should only include what the path selector needs, not a full hardware dump. Extensions go through a privacy review.

---

## 12. 🏗️ Build + deployment

| Item | Details |
|---|---|
| 🧰 Build system | **CMake**, single tree. Phase 0 restructures `unio-pipe/src/` → `src/`, `unio-pipe/include/` → `include/`, plus new `src/orchestrator/` + `src/ui/`. |
| 🎯 Target | One binary per OS: `unio` (Windows `.exe`) / `unio` (Linux ELF, statically linked per [#55]). |
| 📦 Installer | Phase 6 ([#41]) wires signing + packaging. Signed MSI (Windows); `.deb` + `.rpm` + AppImage (Linux). |
| 🐳 Reproducible builds | `packaging/docker/build-*.sh` — both binaries from one Docker-equipped orchestrator host. |

---

## 13. 🟡 Open decisions (next discussion pass)

| # | Status | Decision | Context |
|---|---|---|---|
| 1 | 🔒 LOCKED | **C++ UI toolkit: Dear ImGui (MIT)** — custom-rendered, zero-copy video preview on the same GPU context as the presenter, Mac-ready via Metal | §5, Phase 0 / [#35] |
| 1a | 🔒 LOCKED | **ImGui platform backend: raw Win32 + raw Xlib** — upstream `imgui_impl_win32`, in-house `imgui_impl_x11` (~300–500 LOC), `imgui_impl_osx` ready for Mac. Zero third-party windowing deps. | §5 |
| 1b | 🟡 OPEN | **ImGui rendering backend**: OpenGL 3 portable vs D3D11(Win) + GL(Linux) + Metal(Mac-later) native-per-OS | §5 |
| 2 | 🔒 LOCKED | **Mesh connection model: hybrid** — always-on control connection per paired peer + per-session media connections opened by the orchestrator on `StartStream` | §6 |
| 3 | 🔒 LOCKED | **Peer discovery: mDNS primary + manual invite / QR fallback, no rendezvous server** (enabled by LAN-only scope) | §6 |
| 4 | 🟡 OPEN | **Broadcast cadence + stale thresholds** | §6, §7 |
| 5 | 🟡 OPEN | **Latency-table seeding** for path selector | §8 |
| 6 | 🟡 OPEN | **Multi-viewer fan-out** for single `src_disp` | §9 |
| 7 | 🟡 OPEN | **Reconnection policy** for interrupted QUIC | §9 |
| 8 | 🟡 OPEN | **Capability-record schema version + migration** | §6 |
| 9 | 🟡 OPEN | **`unio-pipe` standalone target post-Phase-3** for `tools/matrix_test.py` + integration tests? | §12 |
| 10 | 🟡 OPEN | **`capture_xcomposite.cpp` hybrid-mode adoption timing** (from `spike/x11-capture-exclude`) — follow-up PR before Phase 3, or during Phase 3? | Separate spike |

Locked this session: **UI category = native C++** (§5), **UI toolkit = Dear ImGui** (§5), **UI platform backend = raw Win32 + raw Xlib** (§5), **LAN-only scope** (§1), **peer discovery = mDNS + invite fallback** (§6), **mesh connection model = hybrid (control + per-session media)** (§6).

---

## 📎 Cross-references

| Resource | Link |
|---|---|
| Scope memo | `project_unio_display_streaming_scope.md` (memory) |
| Coverage matrix | [#31](https://github.com/4d1-1010/Stitch/issues/31) |
| C++ port umbrella | [#34](https://github.com/4d1-1010/Stitch/issues/34) |
| Phase issues | [#35](https://github.com/4d1-1010/Stitch/issues/35) – [#41](https://github.com/4d1-1010/Stitch/issues/41) |
| C++ port live matrix | [#42](https://github.com/4d1-1010/Stitch/issues/42) |
| Distributed capability map | [#63](https://github.com/4d1-1010/Stitch/issues/63) |
| Same-PC swap | [#64](https://github.com/4d1-1010/Stitch/issues/64) |
| `matrix_test.py` post-port | [#65](https://github.com/4d1-1010/Stitch/issues/65) |
| X11 fullscreen carveout | [#62](https://github.com/4d1-1010/Stitch/issues/62) |
| X11 exclusion spike | `spike/x11-capture-exclude` branch |
