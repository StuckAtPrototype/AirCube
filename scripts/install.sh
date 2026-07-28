#!/usr/bin/env bash
# AirCube desktop app installer (Linux / macOS)
set -e

# Move to the directory of this script so relative paths resolve.
cd "$(dirname "$0")"

if command -v python3 >/dev/null 2>&1; then
    PY=python3
elif command -v python >/dev/null 2>&1; then
    PY=python
else
    echo "[ERROR] Python 3 not found on PATH." >&2
    exit 1
fi

exec "$PY" install.py "$@"
