# ZerverMon

ZerverMon is a small TrueNAS server-monitoring project with two parts:

- `zervermon.py` runs on the host machine, collects system and ZFS status, and sends compact JSON updates over a serial connection.
- `zervermon_hw.ino` runs on an ESP32-C3 with a 128x64 OLED and displays rotating status pages for system, ZFS, and temperature information.

## Files

- `zervermon.py`: Python sender for host metrics.
- `zervermon_hw.ino`: ESP32 OLED firmware.
- `zervermon.env`: Runtime configuration such as serial port, polling interval, and pool names.
- `start_zervermon.sh`: Helper script to launch the sender and write logs.

## Configuration

The sender loads settings from `zervermon.env`.

Typical options:

- `SERIAL_PORT`: serial device for the ESP32.
- `INTERVAL_SECONDS`: seconds between payloads.
- `POOL_NAME`: primary ZFS pool for compatibility fields.
- `POOL_NAMES`: comma-separated list of pools to report.
- `LOG_EVERY_N_PAYLOADS`: how often to print payloads to stdout/logs.

## Run

Syntax check:

```bash
python3 -m py_compile /mnt/store/scripts/zervermon/zervermon.py
```

Run the sender directly:

```bash
sudo /mnt/store/scripts/zervermon/zervermon.py
```

Or use the launcher script:

```bash
sudo /mnt/store/scripts/zervermon/start_zervermon.sh
```
