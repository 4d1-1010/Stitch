#!/bin/bash
# Force-kill every unio-pipe helper + clean stale sockets / logs.
# Use when loopback.py's own teardown didn't land or a prior
# run left orphans. Idempotent.
pkill -9 -f 'unio-pipe --socket' 2>/dev/null
sleep 0.3
rm -f /tmp/unio-pipe-*.sock /tmp/unio-sink.log /tmp/unio-src.log
echo "killed helpers + cleaned /tmp/unio-pipe-*.sock + logs"
