#!/usr/bin/env python3
"""UnIO — single UI entry point.

Launching this opens the shell window. Every PC runs a Peer that
auto-discovers other UnIO peers on the LAN — there's no host/join
distinction, no command-line flags.
"""

from __future__ import annotations

import multiprocessing
import sys
from pathlib import Path

# freeze_support goes before anything else so the identify overlays'
# multiprocessing.Process children (used as a fallback when the shell
# can't render Toplevels) don't re-launch the whole app.
multiprocessing.freeze_support()

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))


def main() -> None:
    from unio.apps.shell import main as shell_main
    shell_main()


if __name__ == "__main__":
    main()
