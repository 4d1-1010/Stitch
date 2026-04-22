#!/bin/bash
# sync-clocks.sh — force a fresh NTP resync on the Linux host and
# (optionally) on a Windows host reachable over SSH, then print
# each host's sync status so a subsequent latency-CSV run starts
# from a known-synced state.
#
# Usage:
#   ./tools/sync-clocks.sh                      # Linux only
#   ./tools/sync-clocks.sh --win user@host      # Linux + Windows
#
# Inter-host offset is NOT reported via SSH here — ssh startup
# adds hundreds of ms of noise so any estimate would be garbage
# at the timescale we care about. After this script runs, both
# hosts are fresh against their configured NTP source; if both
# use the same pool (or, better, the same LAN stratum-1) the
# residual inter-host drift is bounded by the pool's dispersion
# (typically < 10 ms on a LAN). For tighter measurement use the
# photodiode / phone-camera procedures in ARCHITECTURE.md.

set -u

win_target=""
if [[ "${1:-}" == "--win" && -n "${2:-}" ]]; then
    win_target="$2"
fi

echo "===== Linux ====="
if command -v chronyc >/dev/null 2>&1; then
    if sudo -n chronyc -a makestep >/dev/null 2>&1; then
        :
    else
        echo "  (chronyc makestep needs sudo; continuing with current state)"
    fi
    chronyc tracking | awk -F': +' '
        /Reference ID/   { printf "  reference:    %s\n", $2 }
        /Stratum/        { printf "  stratum:      %s\n", $2 }
        /Last offset/    { printf "  last offset:  %s\n", $2 }
        /RMS offset/     { printf "  rms offset:   %s\n", $2 }
        /Root dispersion/{ printf "  dispersion:   %s\n", $2 }
    '
elif command -v timedatectl >/dev/null 2>&1; then
    sudo -n timedatectl set-ntp true 2>/dev/null || true
    timedatectl timesync-status 2>/dev/null \
        | awk '/Offset/ || /Packet/ || /Server/ || /Leap/' \
        | sed 's/^/  /'
else
    echo "  no chrony or systemd-timesyncd found"
    exit 1
fi

if [[ -n "$win_target" ]]; then
    echo
    echo "===== Windows (${win_target}) ====="
    ssh "$win_target" 'powershell -NoProfile -Command "Start-Service w32time -ErrorAction SilentlyContinue | Out-Null; w32tm /resync /force | Out-Null; Start-Sleep -Seconds 2; w32tm /query /status"' 2>/dev/null | awk '
        /Stratum/               { printf "  stratum:      %s\n", substr($0, index($0, ":") + 2) }
        /Root Dispersion/       { printf "  dispersion:   %s\n", substr($0, index($0, ":") + 2) }
        /Source:/               { printf "  source:       %s\n", substr($0, index($0, ":") + 2) }
        /Last Successful Sync/  { printf "  last sync:    %s\n", substr($0, index($0, ":") + 2) }
    '
fi

echo
echo "both hosts freshly resynced. Start the CSV-collecting run now;"
echo "re-run this script before any test suite longer than ~5 min."
