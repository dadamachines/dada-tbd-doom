#!/usr/bin/env python3
"""Flash firmware via debug probe and capture UART debug output."""
import serial
import time
import subprocess
import os
import sys

PORT = '/dev/cu.usbmodem1202'
BAUD = 115200
FW_ELF = os.path.join(os.path.dirname(__file__), '.pio/build/doom-tbd16/firmware.elf')

openocd = os.path.expanduser('~/.platformio/packages/tool-openocd-rp2040-earlephilhower/bin/openocd')
scripts = os.path.expanduser('~/.platformio/packages/tool-openocd-rp2040-earlephilhower/share/openocd/scripts')

# Open serial FIRST so we don't miss any boot output
print(f'Opening {PORT} @ {BAUD}...')
ser = serial.Serial(PORT, BAUD, timeout=0.1)
ser.reset_input_buffer()

# Flash + reset via OpenOCD
print(f'Flashing {FW_ELF} via debug probe...')
result = subprocess.run(
    [openocd, '-s', scripts,
     '-f', 'interface/cmsis-dap.cfg', '-f', 'target/rp2350.cfg',
     '-c', 'adapter speed 5000',
     '-c', 'init', '-c', 'reset halt',
     '-c', f'flash write_image erase {FW_ELF}',
     '-c', 'reset run', '-c', 'shutdown'],
    capture_output=True, timeout=30
)
if result.returncode != 0:
    print(f'OpenOCD FAILED (exit {result.returncode}):')
    print(result.stderr.decode('utf-8', errors='replace')[:1000])
    ser.close()
    sys.exit(1)
print('Flashed OK. Capturing UART output for 20 seconds...\n')

output = b''
start = time.time()
while time.time() - start < 20:
    chunk = ser.read(4096)
    if chunk:
        text = chunk.decode('utf-8', errors='replace')
        print(text, end='', flush=True)
        output += chunk

ser.close()
print(f'\n\n=== Total: {len(output)} bytes in {time.time()-start:.1f}s ===')
