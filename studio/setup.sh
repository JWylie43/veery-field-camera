#!/bin/bash
# Create Studio's own Python environment (studio/.venv) and install its dependencies.
set -e
cd "$(dirname "$0")"
python3 -m venv .venv
.venv/bin/pip install --upgrade pip -q
.venv/bin/pip install -r requirements.txt -q
echo "Studio environment ready: $(pwd)/.venv"
