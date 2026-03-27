# DOOM on TBD-16 — User Manual

## Getting Started

1. Power on the TBD-16 — DOOM loads automatically
2. The game starts in **demo mode** (watching the AI play)
3. Press **PLAY** to open the menu
4. Use **UP / DOWN** to navigate, **A** to select
5. Choose **New Game**, pick a skill level, and you're in!

## Controls

```
                ┌──────────────────────────────────┐
                │          TBD-16 D-PAD            │
                │                                  │
                │    [S1]    ↑    [S2]              │
                │        ←  ·  →                   │
                │   [A]     ↓     [B]              │
                │                                  │
                │      [X]  PLAY  REC  [Y]         │
                └──────────────────────────────────┘
```

### Movement

| Button | Action |
|--------|--------|
| **UP** | Move forward |
| **DOWN** | Move backward |
| **LEFT** | Turn left |
| **RIGHT** | Turn right |
| **Y** (hold) + direction | Run (move faster) |
| **X** (hold) + LEFT/RIGHT | Strafe (sidestep) |

### Combat & Interaction

| Button | Action |
|--------|--------|
| **A** | Fire weapon |
| **B** | Use / Open doors / Flip switches |
| **S1** | Previous weapon |
| **S2** | Next weapon |

### System

| Button | Action |
|--------|--------|
| **PLAY** | Pause / Open menu |
| **REC** | Toggle automap |

## Button Identity Reference

The TBD-16 buttons map to DOOM controls as follows:

| TBD-16 Button | Physical Label | MCL Bit | DOOM Key |
|---------------|----------------|---------|----------|
| D-pad UP | ↑ | 3 | Up arrow |
| D-pad DOWN | ↓ | 1 | Down arrow |
| D-pad LEFT | ← | 0 | Left arrow |
| D-pad RIGHT | → | 2 | Right arrow |
| A | F5 | 4 | Right Ctrl (fire) |
| B | F6 | 5 | Space (use) |
| X | Master | 6 | Right Alt (strafe) |
| Y | Sound | 7 | Right Shift (run) |
| PLAY | Play | 8 | Escape (menu) |
| REC | Record | 9 | Tab (automap) |
| S1 | Shift 1 | 10 | , (prev weapon) |
| S2 | Shift 2 | 11 | . (next weapon) |

## Tips

- **Always run**: Hold **Y** while moving to go faster — essential for dodging
- **Strafe to survive**: Hold **X** + LEFT/RIGHT to sidestep enemy fireballs
- **Open everything**: Press **B** on walls that look different — many secret doors
- **Automap**: Press **REC** to see the level map; press again to close
- **Weapon switching**: Use **S1** / **S2** to cycle through collected weapons
- **Save often**: Open the menu with **PLAY** and save your game

## Display

The game renders at **128×64 pixels** on the monochrome SSD1309 OLED. The bottom strip shows the HUD with health, ammo, and armor. See [DITHERING.md](DITHERING.md) for display tuning options.
