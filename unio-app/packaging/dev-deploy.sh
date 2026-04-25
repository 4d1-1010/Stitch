#!/bin/bash
# Build + deploy + launch unio-ui on both adi-pc (this host) and
# Diana (the Windows test box) in one shot. Dev workflow only —
# the live tests need both ends running fresh after every code
# change, this collapses the kill / scp / launch dance into a
# single command.
#
# Usage:
#   unio-app/packaging/dev-deploy.sh                # full cycle
#   unio-app/packaging/dev-deploy.sh --no-build     # skip the
#                                                   # build step
#                                                   # if binaries
#                                                   # are already
#                                                   # current
#
# Diana SSH params come from these env vars (defaults shown):
#   UNIO_WIN_SSH_HOST=192.168.1.18
#   UNIO_WIN_SSH_USER=Diana
#   UNIO_WIN_SSH_KEY=$HOME/.ssh/id_ecdsa
#   UNIO_WIN_REMOTE_DIR=C:\\Users\\Diana\\unio-app

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
UNIO_APP_DIR="$(cd "$HERE/.." && pwd)"

WIN_HOST="${UNIO_WIN_SSH_HOST:-192.168.1.18}"
WIN_USER="${UNIO_WIN_SSH_USER:-Diana}"
WIN_KEY="${UNIO_WIN_SSH_KEY:-$HOME/.ssh/id_ecdsa}"
WIN_REMOTE_DIR='C:\Users\Diana\unio-app'
WIN_TASK="unio-ui-launch"

LINUX_BIN="$UNIO_APP_DIR/dist/linux-x64/unio-ui"
WIN_BIN="$UNIO_APP_DIR/dist/win-x64/unio-ui.exe"

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
# dist/linux-x64/unio-ui — that fails with "text file busy" if
# the previous instance is still alive. Kill before building, not
# after.
echo "###  stopping running instances  ###"
pkill -f "$LINUX_BIN" 2>/dev/null || true
ssh_diana 'taskkill /F /IM unio-ui.exe' 2>&1 \
    | grep -v "ERROR: The process" || true
sleep 1

# ── 2. Build ───────────────────────────────────────────────────
if (( do_build )); then
    echo
    echo "###  building unio-app for Linux + Windows  ###"
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
echo "###  deploying unio-ui.exe to $WIN_USER@$WIN_HOST  ###"
ssh_diana "cmd /c \"if not exist $WIN_REMOTE_DIR mkdir $WIN_REMOTE_DIR\""
# scp wants forward-slashes; the Diana side expands the drive
# letter via OpenSSH's chrooted view.
scp_to_diana "$WIN_BIN" "/C:/Users/Diana/unio-app/"

# ── 4. Launch on both ──────────────────────────────────────────
echo
echo "###  launching on $WIN_USER@$WIN_HOST  ###"
# schtasks /IT pins the GUI to the logged-on user's interactive
# session — without it the window opens in Session 0 and stays
# invisible (see reference_diana memory).
ssh_diana "schtasks /Run /TN $WIN_TASK"

echo
echo "###  launching on $(hostname)  ###"
"$LINUX_BIN" >/tmp/unio-ui.log 2>&1 &
disown
sleep 2

# ── 5. Quick liveness check ────────────────────────────────────
echo
echo "###  alive?  ###"
if pgrep -f "$LINUX_BIN" >/dev/null; then
    echo "  $(hostname): unio-ui pid $(pgrep -f "$LINUX_BIN" | head -1)"
else
    echo "  $(hostname): unio-ui NOT running" >&2
fi
echo "  $WIN_USER@$WIN_HOST: $(ssh_diana 'tasklist | findstr unio-ui' \
    | tr -s ' ' || echo NOT_RUNNING)"
