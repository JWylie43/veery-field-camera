#!/bin/bash
# Double-click to start Veery Studio (opens the browser). Ctrl+C or close the window to stop.
cd "$(dirname "$0")"
[ -x .venv/bin/python ] || ./setup.sh
exec .venv/bin/python server.py "$@"
