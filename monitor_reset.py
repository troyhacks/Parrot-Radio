#!/usr/bin/env python3
"""
ESP32-S3 Reboot + Monitor Script

Opens the serial monitor BEFORE resetting the device, then triggers a reset.
On ESP32-S3 with USB CDC, the ROM bootloader re-enumerates the COM port after
a soft reset, so we can monitor through the entire boot sequence.

Usage: python monitor_reset.py [COM_PORT] [BAUD]
Defaults: COM39, 115200
"""

import sys
import time
import subprocess
import threading
import re

# Try pyserial, fall back to subprocess
try:
    import serial
    HAS_SERIAL = True
except ImportError:
    HAS_SERIAL = False
    print("Note: pyserial not available, using pio device monitor fallback")

DEFAULT_PORT = "COM39"
DEFAULT_BAUD = 115200


def find_port_after_reset(original_port, timeout=10):
    """Wait for a COM port to appear after device reset."""
    if not HAS_SERIAL:
        return original_port

    import serial.tools.list_ports
    start = time.time()
    while time.time() - start < timeout:
        ports = list(serial.tools.list_ports.comports())
        for p in ports:
            # Look for the same device (by VID/PID/SER) or any ESP32-S3
            if "303A:1001" in p.hwid:  # ESP32-S3 USB VID:PID
                print(f"  Found re-enumerated port: {p.device}")
                return p.device
        time.sleep(0.5)

    print(f"  Warning: Port didn't re-enumerate, using original: {original_port}")
    return original_port


def monitor_with_pyserial(port, baud, output_collector):
    """Monitor serial port and collect output."""
    try:
        ser = serial.Serial(port, baud, timeout=1)
        ser.dtr = False
        ser.rts = False
        time.sleep(0.1)

        # Toggle RTS to reset (this triggers the ESP32-S3 bootloader)
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
        ser.dtr = True  # Enable DTR (normal operation)

        print(f"  Reset triggered, monitoring {port}...")
        start = time.time()

        while time.time() - start < 15:  # Monitor for 15 seconds
            if ser.in_waiting:
                line = ser.readline().decode('utf-8', errors='replace')
                output_collector.append(line)
                print(line, end='')  # Real-time output
            time.sleep(0.01)

        ser.close()
        print("\n  Monitoring complete.")

    except Exception as e:
        print(f"  pyserial error: {e}")
        output_collector.append(f"ERROR: {e}\n")


def monitor_with_pio_fallback(port, baud, output_collector):
    """Use PlatformIO device monitor as fallback."""
    # Kill any existing pio processes
    subprocess.run(["taskkill", "/F", "/IM", "python.exe"], capture_output=True)
    time.sleep(1)

    # Start pio device monitor in background
    pio_cmd = [
        "C:\\Users\\troy\\.platformio\\penv\\Scripts\\pio.exe",
        "device", "monitor", "-e", "ttwr", "--raw"
    ]
    print(f"  Starting pio monitor: {' '.join(pio_cmd)}")
    proc = subprocess.Popen(pio_cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           universal_newlines=True, bufsize=1)

    # Wait for monitor to connect
    time.sleep(2)

    # Trigger reset via esptool
    esptool_cmd = [
        "C:\\Users\\troy\\.platformio\\packages\\tool-esptoolpy\\esptool.py",
        "--port", port, "run"
    ]
    print(f"  Triggering reset via esptool: {' '.join(esptool_cmd)}")
    subprocess.run(esptool_cmd, capture_output=True)

    # Collect output for a few seconds
    time.sleep(8)
    proc.terminate()
    output, _ = proc.communicate(timeout=5)
    output_collector.append(output)
    print(output)


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PORT
    baud = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_BAUD

    print(f"ESP32-S3 Reboot + Monitor")
    print(f"Port: {port}, Baud: {baud}")
    print("=" * 50)

    output = []

    if HAS_SERIAL:
        monitor_with_pyserial(port, baud, output)
    else:
        monitor_with_pio_fallback(port, baud, output)

    # Extract key lines
    print("\n" + "=" * 50)
    print("KEY CALIBRATION DATA:")
    print("=" * 50)
    full_output = "".join(output)

    patterns = [
        r"ADC calibration: \d+ samples",
        r"ADC actual rate: \d+ Hz",
        r"ADC DC offset: \d+",
        r"Using nearest valid rate.*",
        r"Rate converter initialized: \d+ Hz",
        r"Goertzel initialized for [\d.]+ Hz",
    ]

    for line in full_output.split('\n'):
        for p in patterns:
            if re.search(p, line):
                print(f"  {line.strip()}")
                break

    return 0


if __name__ == "__main__":
    sys.exit(main())
