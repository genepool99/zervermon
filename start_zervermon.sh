#!/usr/bin/env bash

set -u

APP_DIR="/mnt/store/scripts/zervermon"
PYTHON_BIN="$APP_DIR/.venv/bin/python"
SCRIPT="$APP_DIR/zervermon.py"
LOG_FILE="$APP_DIR/zervermon.log"

MAX_LOG_BYTES=1048576  # 1 MB

if [ -f "$LOG_FILE" ]; then
  LOG_SIZE=$(stat -c%s "$LOG_FILE" 2>/dev/null || echo 0)
  if [ "$LOG_SIZE" -gt "$MAX_LOG_BYTES" ]; then
    mv "$LOG_FILE" "$LOG_FILE.1"
  fi
fi

cd "$APP_DIR" || exit 1

echo "========================================" >> "$LOG_FILE"
echo "Starting ZerverMon at $(date)" >> "$LOG_FILE"
echo "Running as user: $(whoami)" >> "$LOG_FILE"
echo "App dir: $APP_DIR" >> "$LOG_FILE"

if [ -x "$PYTHON_BIN" ]; then
  echo "Using venv Python: $PYTHON_BIN" >> "$LOG_FILE"
  exec "$PYTHON_BIN" -u "$SCRIPT" >> "$LOG_FILE" 2>&1
else
  echo "Venv Python not found, using system python3" >> "$LOG_FILE"
  exec /usr/bin/env python3 -u "$SCRIPT" >> "$LOG_FILE" 2>&1
fi