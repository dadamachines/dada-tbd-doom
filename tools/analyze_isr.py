#!/usr/bin/env python3
"""Analyze ISR debug output from flash_and_capture."""
import re, sys

path = sys.argv[1] if len(sys.argv) > 1 else "/Users/jlo/Library/Application Support/Code/User/workspaceStorage/ef7072d61876900a4fe7a5c6f925429f/GitHub.copilot-chat/chat-session-resources/5cfacded-e6fc-409b-8203-c70ad400947e/toolu_vrtx_01VV8FEAxuzm3oz6J36op3mb__vscode-1774597186192/content.txt"

lines = open(path).readlines()
vals = []
for l in lines:
    m = re.search(r'sent=(\d+)\s+isr=(\d+)\s+dma_busy=(\d+)\s+rdy_low=(\d+)', l)
    if m:
        vals.append(tuple(int(x) for x in m.groups()))

if not vals:
    print("No ISR stats found")
    sys.exit(1)

# Find first post-boot entry (sent counter resets to small value)
boot_idx = 0
for i in range(1, len(vals)):
    if vals[i][0] < vals[i-1][0]:
        boot_idx = i
        break

post = vals[boot_idx:]
print(f"Post-boot entries: {len(post)}")
print(f"First: sent={post[0][0]} isr={post[0][1]}")
print(f"Last:  sent={post[-1][0]} isr={post[-1][1]}")
print()

s, i, d, r = post[-1]
print(f"Total frames sent: {s}")
print(f"Total ISR fires:   {i}")
print(f"ISR fire rate:     {i/60:.0f} Hz (expect ~5000)")
print(f"Frame send rate:   {s/60:.0f} Hz (need ~1378)")
print(f"Hit rate:          {s/i*100:.1f}%")
print(f"DMA busy skips:    {d} ({d/i*100:.1f}%)")
print(f"RDY low skips:     {r} ({r/i*100:.1f}%)")
print()

# Show deltas for last 10
print("Last 10 deltas between prints:")
for idx in range(-10, 0):
    if idx-1 >= -len(post):
        ds = post[idx][0] - post[idx-1][0]
        di = post[idx][1] - post[idx-1][1]
        dd = post[idx][2] - post[idx-1][2]
        dr = post[idx][3] - post[idx-1][3]
        print(f"  sent_delta={ds:4d}  isr_delta={di:5d}  dma_busy={dd:4d}  rdy_low={dr:5d}")
