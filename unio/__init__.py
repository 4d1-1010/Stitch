"""UnIO - Cross-machine distributed input routing system."""

import os

__version__ = "0.0.2-rebrand-unio"

# When False (default, production builds), the log buffer isn't
# installed and the "View logs" affordances don't render — end users
# don't see diagnostic UI they don't need. Flip to True by either:
#   * building with `python packaging/build.py --dev-logs`, which
#     patches this constant in place before PyInstaller runs, or
#   * setting UNIO_DEV_LOGS=1 in the environment at runtime (no
#     rebuild needed — good for ad-hoc debugging of a release build).
DEV_LOGS = bool(os.environ.get("UNIO_DEV_LOGS"))
