#!/usr/bin/env python3
"""
Stitch — Display Configuration Tool

A native desktop application to visually arrange displays from all
connected machines. Drag displays to match your physical layout.

Usage:
    python run_configurator.py [--server HOST] [--port PORT]
"""

from ud.configurator import main

if __name__ == "__main__":
    main()
