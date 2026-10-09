#!/bin/bash
# Double-click to start Studio (opens the browser). Ctrl+C or close the window to stop.
# Uses the repo's Python environment (.venv at the repo root); runs ../setup.sh first if
# that doesn't exist yet.
cd "$(dirname "$0")"
[ -x ../.venv/bin/python ] || ../setup.sh
exec ../.venv/bin/python server.py "$@"
