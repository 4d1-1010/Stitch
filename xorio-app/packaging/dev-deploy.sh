#!/bin/bash
# Build + deploy + launch xorio on both adi-pc (this host) and
# Diana (the Windows test box) in one shot. Dev workflow only —
# the live tests need both ends running fresh after every code
# change, this collapses the kill / scp / launch dance into a
# single command.
#
# Usage:
#   xorio-app/packaging/dev-deploy.sh                # full cycle
#   xorio-app/packaging/dev-deploy.sh --no-build     # skip the
#                                                   # build step
#                                                   # if binaries
#                                                   # are already
#                                                   # current
#
# Diana SSH params come from these env vars (defaults shown):
#   XORIO_WIN_SSH_HOST=192.168.1.22
#   XORIO_WIN_SSH_USER=Diana
#   XORIO_WIN_SSH_KEY=$HOME/.ssh/id_ecdsa
#   XORIO_WIN_REMOTE_DIR=C:\\Users\\Diana\\xorio-app

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
XORIO_APP_DIR="$(cd "$HERE/.." && pwd)"

WIN_HOST="${XORIO_WIN_SSH_HOST:-192.168.1.22}"
WIN_USER="${XORIO_WIN_SSH_USER:-Diana}"
WIN_KEY="${XORIO_WIN_SSH_KEY:-$HOME/.ssh/id_ecdsa}"
WIN_REMOTE_DIR='C:\Users\Diana\xorio-app'
WIN_TASK="xorio-launch"

LINUX_BIN="$XORIO_APP_DIR/dist/linux-x64/xorio"
WIN_BIN="$XORIO_APP_DIR/dist/win-x64/xorio.exe"

ssh_diana() {
    ssh -o IdentitiesOnly=yes -i "$WIN_KEY" "$WIN_USER@$WIN_HOST" "$@"
}

scp_to_diana() {
    scp -o IdentitiesOnly=yes -i "$WIN_KEY" "$1" "$WIN_USER@$WIN_HOST:$2"
}

do_build=1
for arg in "$@"; do
    case "$arg" in
        --no-build) do_build=0 ;;
        *) echo "unknown flag: $arg" >&2; exit 2 ;;
    esac
done

# ── 1. Stop running instances FIRST ────────────────────────────
# The Linux extract step in build-linux.sh `cp`'s the binary into
# dist/linux-x64/xorio — that fails with "text file busy" if
# the previous instance is still alive. Kill before building, not
# after.
echo "###  stopping running instances  ###"
pkill -f "$LINUX_BIN" 2>/dev/null || true
ssh_diana 'taskkill /F /IM xorio.exe' 2>&1 \
    | grep -v "ERROR: The process" || true
sleep 1

# ── 2. Build ───────────────────────────────────────────────────
if (( do_build )); then
    echo
    echo "###  building xorio-app for Linux + Windows  ###"
    "$HERE/build-all.sh"
fi

if [[ ! -x "$LINUX_BIN" ]]; then
    echo "error: $LINUX_BIN missing — run with build enabled." >&2
    exit 1
fi
if [[ ! -f "$WIN_BIN" ]]; then
    echo "error: $WIN_BIN missing — run with build enabled." >&2
    exit 1
fi

# ── 3. Deploy to Diana ─────────────────────────────────────────
echo
echo "###  deploying xorio.exe to $WIN_USER@$WIN_HOST  ###"
ssh_diana "cmd /c \"if not exist $WIN_REMOTE_DIR mkdir $WIN_REMOTE_DIR\""
# scp wants forward-slashes; the Diana side expands the drive
# letter via OpenSSH's chrooted view.
scp_to_diana "$WIN_BIN" "/C:/Users/Diana/xorio-app/"

# ── 4. Launch on both ──────────────────────────────────────────
echo
echo "###  launching on $WIN_USER@$WIN_HOST  ###"
# schtasks /IT pins the GUI to the logged-on user's interactive
# session — without it the window opens in Session 0 and stays
# invisible (see reference_diana memory).
ssh_diana "schtasks /Run /TN $WIN_TASK"

echo
echo "###  launching on $(hostname)  ###"
"$LINUX_BIN" >/tmp/xorio.log 2>&1 &
disown
sleep 2

# ── 5. Quick liveness check ────────────────────────────────────
echo
echo "###  alive?  ###"
if pgrep -f "$LINUX_BIN" >/dev/null; then
    echo "  $(hostname): xorio pid $(pgrep -f "$LINUX_BIN" | head -1)"
else
    echo "  $(hostname): xorio NOT running" >&2
fi
echo "  $WIN_USER@$WIN_HOST: $(ssh_diana 'tasklist | findstr xorio' \
    | tr -s ' ' || echo NOT_RUNNING)"
