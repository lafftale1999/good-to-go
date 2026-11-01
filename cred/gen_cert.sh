#!/bin/sh
#
# macOS/Linux entry point. The work is done by gen_cert.py so the result is the
# same on every OS. See gen_cert.py for the outputs.
#
# Usage: ./gen_cert.sh [--force-ca]

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

if command -v python3 >/dev/null 2>&1; then
    PYTHON=python3
elif command -v python >/dev/null 2>&1; then
    PYTHON=python
else
    echo "error: Python 3 not found. Run this from an ESP-IDF terminal (. ./export.sh)." >&2
    exit 1
fi

exec "$PYTHON" "$SCRIPT_DIR/gen_cert.py" "$@"
