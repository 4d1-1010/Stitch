#!/usr/bin/env python3
"""
UnIO — Display Configuration Tool

A native desktop application to visually arrange displays from all
connected machines. Drag displays to match your physical layout.

Usage:
    python run_configurator.py [--server HOST] [--port PORT]
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from unio.apps.configurator import main

if __name__ == "__main__":
    main()
