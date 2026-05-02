#!/usr/bin/env python3
"""Collect host health data and stream it over serial to an ESP32 display."""

import json
import os
import re
import socket
import subprocess
import time
from datetime import datetime
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
ENV_FILE = SCRIPT_DIR / "zervermon.env"


def load_env_file(path: Path) -> None:
    """Load environment variables from a file if it exists."""
    if not path.exists():
        return

    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()

        if not line or line.startswith("#"):
            continue

        if "=" not in line:
            continue

        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip().strip('"').strip("'")

        if key and key not in os.environ:
            os.environ[key] = value


load_env_file(ENV_FILE)


def get_env_int(name: str, default: int) -> int:
    """Return an integer environment variable value with a safe fallback."""
    try:
        return int(os.getenv(name, str(default)))
    except ValueError:
        return default


SERIAL_PORT = os.getenv(
    "SERIAL_PORT",
    "/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_AC:A7:04:B9:2C:F0-if00",
)

INTERVAL_SECONDS = get_env_int("INTERVAL_SECONDS", 5)
LOG_EVERY_N_PAYLOADS = get_env_int("LOG_EVERY_N_PAYLOADS", 60)
MAX_POOLS = get_env_int("MAX_POOLS", 4)

_pool_name = os.getenv("POOL_NAME", "").strip()
POOL_NAME = _pool_name if _pool_name else None

_pool_names = os.getenv("POOL_NAMES", "").strip()
POOL_NAMES = [name.strip() for name in _pool_names.split(",")
              if name.strip()] if _pool_names else None

NET_LAST: dict[str, tuple[int, int, float]] = {}


def run(cmd: list[str], timeout: int = 3) -> str:
    """Run a command and return stripped stdout, or an empty string on failure."""
    try:
        return subprocess.check_output(
            cmd,
            text=True,
            stderr=subprocess.DEVNULL,
            timeout=timeout,
        ).strip()
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired, OSError):
        return ""


def log(message: str) -> None:
    """Print a timestamped local log message."""
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    print(f"[{timestamp}] {message}", flush=True)


def get_hostname() -> str:
    """Return the short hostname for the current machine."""
    return socket.gethostname().split(".")[0] or "server"


def get_ip() -> str:
    """Return the primary IPv4 address, if one can be detected."""
    output = run(
        ["sh", "-c", "ip -4 route get 1.1.1.1 | awk '{print $7; exit}'"])
    if output:
        return output

    output = run(["hostname", "-I"])
    if output:
        return output.split()[0]

    return "-"


def get_load() -> str:
    """Return compact system load averages for 1, 5, and 15 minutes."""
    try:
        with open("/proc/loadavg", "r", encoding="utf-8") as f:
            parts = f.read().split()
        if len(parts) >= 3:
            return " ".join(parts[:3])
        if parts:
            return parts[0]
    except OSError:
        pass
    return "-"


def get_ram_percent() -> str:
    """Return RAM usage as a whole-number percentage string."""
    try:
        meminfo = {}
        with open("/proc/meminfo", "r", encoding="utf-8") as f:
            for line in f:
                key, value = line.split(":", 1)
                meminfo[key] = int(value.strip().split()[0])

        total = meminfo.get("MemTotal", 0)
        available = meminfo.get("MemAvailable", 0)

        if total <= 0:
            return "-"

        used_percent = round((total - available) / total * 100)
        return f"{used_percent}%"
    except (OSError, ValueError):
        return "-"


def get_pool_name() -> str:
    """Return the configured ZFS pool or the first non-boot pool found."""
    if POOL_NAME:
        return POOL_NAME

    output = run(["zpool", "list", "-H", "-o", "name"])
    pools = [line.strip() for line in output.splitlines() if line.strip()]

    if not pools:
        return ""

    for pool in pools:
        if "boot" not in pool.lower():
            return pool

    return pools[0]


def get_all_pool_names() -> list[str]:
    """Return all configured or detected non-boot ZFS pool names."""
    if POOL_NAMES:
        return POOL_NAMES

    output = run(["zpool", "list", "-H", "-o", "name"])
    pools = [line.strip() for line in output.splitlines() if line.strip()]

    return [pool for pool in pools if "boot" not in pool.lower()]


def get_pool_health(pool: str) -> str:
    """Return the health status for a ZFS pool."""
    if not pool:
        return "-"

    return run(["zpool", "list", "-H", "-o", "health", pool]) or "-"


def get_pool_used_total(pool: str) -> tuple[str, str]:
    """Return allocated and total sizes for a ZFS pool."""
    if not pool:
        return "-", "-"

    output = run(["zpool", "list", "-H", "-o", "allocated,size", pool])
    parts = output.split()

    if len(parts) >= 2:
        return parts[0], parts[1]

    return "-", "-"


def get_pool_summary_line(pool: str) -> str:
    """Return a compact health and usage summary for one ZFS pool."""
    output = run(["zpool", "list", "-H", "-o",
                 "name,health,allocated,size", pool])

    if not output:
        return f"{pool} -"

    parts = output.split()
    if len(parts) < 4:
        return f"{pool} -"

    name, health, used, total = parts[0], parts[1], parts[2], parts[3]

    return f"{name} {health} {used}/{total}"


def get_pool_detail(pool: str) -> dict[str, str]:
    """Return structured pool details for one ZFS pool."""
    output = run(["zpool", "list", "-H", "-o", "name,health,allocated,size,capacity", pool])

    if not output:
        return {
            "name": pool or "-",
            "health": "-",
            "used": "-",
            "total": "-",
            "capacity": "-",
        }

    parts = output.split()
    if len(parts) < 5:
        return {
            "name": pool or "-",
            "health": "-",
            "used": "-",
            "total": "-",
            "capacity": "-",
        }

    return {
        "name": parts[0],
        "health": parts[1],
        "used": parts[2],
        "total": parts[3],
        "capacity": parts[4],
    }


def get_pool_overall_health(pools: list[str]) -> str:
    """Collapse per-pool health into a single status for the payload."""
    if not pools:
        return "-"

    health_values = [get_pool_health(pool) for pool in pools]

    if any(health not in ("ONLINE", "-") for health in health_values):
        return "WARN"

    if all(health == "ONLINE" for health in health_values):
        return "ONLINE"

    return "-"


def get_cpu_temp() -> str:
    """Return the preferred CPU temperature reading in Celsius."""
    output = run(["sensors"], timeout=4)

    if output:
        match = re.search(
            r"CPU0 Temperature:\s+\+([0-9]+(?:\.[0-9]+)?)°C",
            output,
        )
        if match:
            return f"{round(float(match.group(1)))}C"

        # Fallback to the CPU package temperature from coretemp.
        match = re.search(
            r"Package id \d+:\s+\+([0-9]+(?:\.[0-9]+)?)°C",
            output,
        )
        if match:
            return f"{round(float(match.group(1)))}C"

        # Fallback to individual core temperatures.
        core_temps = []
        for match in re.finditer(
            r"Core \d+:\s+\+([0-9]+(?:\.[0-9]+)?)°C",
            output,
        ):
            try:
                core_temps.append(float(match.group(1)))
            except ValueError:
                pass

        if core_temps:
            return f"{round(max(core_temps))}C"

    # Fallback: Linux thermal zones.
    for path in Path("/sys/class/thermal").glob("thermal_zone*/temp"):
        try:
            type_path = path.with_name("type")
            zone_type = type_path.read_text(encoding="utf-8").strip()

            # Prefer actual x86 CPU package thermal zone.
            if zone_type != "x86_pkg_temp":
                continue

            raw = path.read_text(encoding="utf-8").strip()
            value = int(raw)

            if value > 1000:
                value = value / 1000

            if 0 < value < 120:
                return f"{round(value)}C"
        except (OSError, ValueError):
            pass

    return "-"


def get_ambient_temp() -> str:
    """Return the ambient system temperature when exposed by lm-sensors."""
    output = run(["sensors"], timeout=4)
    if not output:
        return "-"

    match = re.search(
        r"System Ambient Temperature:\s+\+([0-9]+(?:\.[0-9]+)?)°C",
        output,
    )
    if match:
        return f"{round(float(match.group(1)))}C"

    return "-"


def get_system_time() -> str:
    """Return the current local system time in hours and minutes."""
    return datetime.now().strftime("%H:%M")


def get_system_date() -> str:
    """Return the current local system date."""
    return datetime.now().strftime("%Y-%m-%d")


def get_uptime() -> str:
    """Return a compact uptime string."""
    try:
        with open("/proc/uptime", "r", encoding="utf-8") as file_handle:
            seconds = int(float(file_handle.read().split()[0]))

        days, remainder = divmod(seconds, 86400)
        hours, remainder = divmod(remainder, 3600)
        minutes, _ = divmod(remainder, 60)

        if days > 0:
            return f"{days}d {hours}h"
        if hours > 0:
            return f"{hours}h {minutes}m"
        return f"{minutes}m"
    except (OSError, ValueError, IndexError):
        return "-"


def get_logged_in_users() -> str:
    """Return the number of unique logged-in usernames."""
    output = run(["who"])
    if not output:
        return "0"

    users = set()
    for line in output.splitlines():
        parts = line.split()
        if parts:
            users.add(parts[0])

    return str(len(users))


def get_default_iface() -> str:
    """Return the default IPv4 egress interface name."""
    output = run(["sh", "-c", "ip -4 route get 1.1.1.1 | awk '{print $5; exit}'"])
    return output or "-"


def get_link_speed(iface: str) -> str:
    """Return link speed for an interface in megabits per second."""
    if not iface or iface == "-":
        return "-"

    path = Path("/sys/class/net") / iface / "speed"
    try:
        speed = path.read_text(encoding="utf-8").strip()
        if speed and speed != "-1":
            return f"{speed}M"
    except OSError:
        pass

    return "-"


def read_net_bytes(iface: str) -> tuple[int, int] | None:
    """Return RX and TX byte counters for an interface."""
    if not iface or iface == "-":
        return None

    base = Path("/sys/class/net") / iface / "statistics"

    try:
        rx = int((base / "rx_bytes").read_text(encoding="utf-8").strip())
        tx = int((base / "tx_bytes").read_text(encoding="utf-8").strip())
        return rx, tx
    except (OSError, ValueError):
        return None


def format_rate(bytes_per_second: float) -> str:
    """Format a byte rate for compact display."""
    if bytes_per_second >= 1024 * 1024:
        return f"{bytes_per_second / (1024 * 1024):.1f}M/s"
    if bytes_per_second >= 1024:
        return f"{bytes_per_second / 1024:.0f}K/s"
    return f"{bytes_per_second:.0f}B/s"


def get_net_rates(iface: str) -> tuple[str, str]:
    """Return RX and TX rates for an interface using cached byte counters."""
    now = time.monotonic()
    current = read_net_bytes(iface)

    if current is None:
        return "-", "-"

    rx, tx = current
    previous = NET_LAST.get(iface)
    NET_LAST[iface] = (rx, tx, now)

    if previous is None:
        return "0B/s", "0B/s"

    prev_rx, prev_tx, prev_time = previous
    elapsed = max(now - prev_time, 0.001)

    rx_rate = max((rx - prev_rx) / elapsed, 0)
    tx_rate = max((tx - prev_tx) / elapsed, 0)

    return format_rate(rx_rate), format_rate(tx_rate)


def get_disk_max_temp() -> str:
    """Return the highest SMART-reported disk temperature in Celsius."""
    temps = []

    devices = run(["lsblk", "-dn", "-o", "NAME,TYPE"])
    for line in devices.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue

        name, dev_type = parts[0], parts[1]
        if dev_type != "disk":
            continue

        dev = f"/dev/{name}"
        output = run(["smartctl", "-A", dev], timeout=5)
        if not output:
            continue

        for smart_line in output.splitlines():
            if "Temperature_Celsius" in smart_line or "Airflow_Temperature_Cel" in smart_line:
                fields = smart_line.split()
                for field in reversed(fields):
                    if field.isdigit():
                        value = int(field)
                        if 0 < value < 120:
                            temps.append(value)
                            break

        match = re.search(r"Temperature:\s+([0-9]+)\s+Celsius", output)
        if match:
            value = int(match.group(1))
            if 0 < value < 120:
                temps.append(value)

    if temps:
        return f"{max(temps)}C"

    return "-"


def get_hp_fans() -> dict[str, str]:
    """Return HP WMI fan RPM values keyed by cpu/rear/front/mem."""
    output = run(["sensors"], timeout=4)
    if not output:
        return {}

    fan_map = {
        "cpu": "N/A",
        "rear": "N/A",
        "front": "N/A",
        "mem": "N/A",
    }

    patterns = {
        "cpu": r"CPU0 Fan:\s+([0-9]+)\s+RPM",
        "rear": r"Rear Chassis Fan0:\s+([0-9]+)\s+RPM",
        "front": r"Front Chassis Fan0:\s+([0-9]+)\s+RPM",
        "mem": r"Memory Fan0:\s+([0-9]+)\s+RPM",
    }

    for key, pattern in patterns.items():
        match = re.search(pattern, output)
        if match:
            fan_map[key] = match.group(1)

    return fan_map


def get_fan_status(fans: dict[str, str] | None = None) -> str:
    """Return a compact min-max summary of detected fan RPM values."""
    if fans is None:
        fans = get_hp_fans()

    rpms = []
    for value in fans.values():
        if value.isdigit():
            rpms.append(int(value))

    if not rpms:
        output = run(["sensors"], timeout=4)
        for match in re.finditer(r":\s+([0-9]+)\s+RPM", output):
            try:
                rpms.append(int(match.group(1)))
            except ValueError:
                pass

    if not rpms:
        return "N/A"

    if len(rpms) == 1:
        return str(rpms[0])

    return f"{min(rpms)}-{max(rpms)}"


def build_payload() -> dict[str, str]:
    """Assemble the serial payload for the ESP32 display."""
    fans = get_hp_fans()
    pools = get_all_pool_names()

    primary_pool = POOL_NAME
    if not primary_pool and pools:
        primary_pool = pools[0]

    used, total = get_pool_used_total(
        primary_pool) if primary_pool else ("-", "-")

    zfs_lines = [get_pool_summary_line(pool) for pool in pools]

    while len(zfs_lines) < 3:
        zfs_lines.append("-")

    iface = get_default_iface()
    net_rx, net_tx = get_net_rates(iface)

    payload = {
        "hostname": get_hostname(),
        "ip": get_ip(),
        "load": get_load(),
        "pool": get_pool_overall_health(pools),
        "temp": get_cpu_temp(),
        "ambient": get_ambient_temp(),
        "ram": get_ram_percent(),
        "used": used,
        "total": total,
        "disk": get_disk_max_temp(),
        "fan": get_fan_status(fans),
        "fan_cpu": fans.get("cpu", "N/A"),
        "fan_rear": fans.get("rear", "N/A"),
        "fan_front": fans.get("front", "N/A"),
        "fan_mem": fans.get("mem", "N/A"),
        "zfs1": zfs_lines[0],
        "zfs2": zfs_lines[1],
        "zfs3": zfs_lines[2],
        "uptime": get_uptime(),
        "users": get_logged_in_users(),
        "time": get_system_time(),
        "date": get_system_date(),
        "net_iface": iface,
        "net_speed": get_link_speed(iface),
        "net_rx": net_rx,
        "net_tx": net_tx,
        "pool_count": str(min(len(pools), MAX_POOLS)),
    }

    selected_pools = pools[:MAX_POOLS]

    for index in range(MAX_POOLS):
        slot = index + 1

        if index < len(selected_pools):
            detail = get_pool_detail(selected_pools[index])
        else:
            detail = {
                "name": "-",
                "health": "-",
                "used": "-",
                "total": "-",
                "capacity": "-",
            }

        payload[f"pool{slot}_name"] = detail["name"]
        payload[f"pool{slot}_health"] = detail["health"]
        payload[f"pool{slot}_used"] = detail["used"]
        payload[f"pool{slot}_total"] = detail["total"]
        payload[f"pool{slot}_capacity"] = detail["capacity"]

    return payload


def main() -> None:
    """Continuously publish host metrics to the configured serial device."""
    log(f"ESP32 OLED sender starting. Serial port: {SERIAL_PORT}")

    payload_count = 0

    while True:
        if not os.path.exists(SERIAL_PORT):
            log(f"Waiting for serial port: {SERIAL_PORT}")
            time.sleep(2)
            continue

        try:
            with open(SERIAL_PORT, "w", encoding="utf-8", buffering=1) as serial:
                log("Serial port opened.")

                while True:
                    payload = build_payload()
                    line = json.dumps(payload, separators=(",", ":"))

                    payload_count += 1
                    if LOG_EVERY_N_PAYLOADS <= 1 or payload_count % LOG_EVERY_N_PAYLOADS == 0:
                        log(line)

                    serial.write(line + "\n")
                    serial.flush()

                    time.sleep(INTERVAL_SECONDS)

        except KeyboardInterrupt:
            log("Stopped.")
            return
        except OSError as exc:
            log(f"Serial/write error: {exc}")
            time.sleep(2)


if __name__ == "__main__":
    main()
