#!/usr/bin/env python3
"""Flash RP2350 and capture boot output from debug probe UART."""
import serial
import subprocess
import sys
import time
import threading

PORT = '/dev/cu.usbmodem14302'
BAUD = 115200
PROJECT = '/Users/jlo/Documents/GitHub/dada-tbd-doom'

lines = []
stop_flag = threading.Event()

def reader_thread(port, baud):
    """Read serial in background, collecting lines."""
    try:
        s = serial.Serial(port, baud, timeout=0.3)
        s.reset_input_buffer()
        print(f"[monitor] Serial open on {port}")
        while not stop_flag.is_set():
            data = s.readline()
            if data:
                line = data.decode('utf-8', errors='replace').rstrip()
                lines.append(line)
                print(f"  > {line}")
        s.close()
    except Exception as e:
        print(f"[monitor] Serial error: {e}")

# 1) Start serial reader FIRST
t = threading.Thread(target=reader_thread, args=(PORT, BAUD), daemon=True)
t.start()
time.sleep(0.5)

# 2) Flash (this resets the device)
print("[monitor] Flashing...")
result = subprocess.run(
    ['pio', 'run', '-t', 'upload'],
    capture_output=True, text=True, timeout=60,
    cwd=PROJECT
)
if result.returncode != 0:
    print("[monitor] Flash FAILED!")
    print(result.stderr[-300:])
    stop_flag.set()
    sys.exit(1)

print("[monitor] Flash OK! Capturing boot output for 60s...")
time.sleep(60)

# 3) Stop and report
stop_flag.set()
t.join(timeout=2)

print("\n" + "="*60)
print("CAPTURED BOOT OUTPUT:")
print("="*60)
for l in lines:
    print(l)
print("="*60)
print(f"Total lines captured: {len(lines)}")
