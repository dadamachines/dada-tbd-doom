# TBD-16 Button & Control Mapping

## Physical Layout (Front Panel)

```
    ┌─────────────────────────────────────────────────────────┐
    │ (screw)                                        (screw)  │
    │           ┌────────────────────────────┐                │
    │           │                            │                │
    │  POT_L    │     128 × 64 OLED          │      POT_R    │
    │   ◉       │     (SSD1309)              │        ◉      │
    │           └────────────────────────────┘                │
    │ (screw)                                        (screw)  │
    │                                                         │
    │  ◉           ◉        ◉        ◉           ◉           │
    │ POT1        POT2     POT3     POT4         POT5         │
    │                                                         │
    │              🔴 REC      ⚪ MASTER                      │
    │              ⚪ PLAY     🔵 SOUND                       │
    │                                                         │
    │  ⊙  ⊙  ⊙  ⊙  ⊙  ⊙  ⊙  ⊙                            │
    │  D1  D2  D3  D4  D5  D6  D7  D8                        │
    │  ⊙  ⊙  ⊙  ⊙  ⊙  ⊙  ⊙  ⊙                            │
    │  D9 D10 D11 D12 D13 D14 D15 D16                        │
    │                                                         │
    │  ◉   F5  ↑  ←  ↓  →  F6   ◉                           │
    │  F1       [  D-PAD  ]       F2                          │
    │         S1  PLAY  REC  S2                               │
    └─────────────────────────────────────────────────────────┘
```

## Input Sources

All buttons except the Favorite button are read via **I2C** from the **STM32 UI board**.

| Interface | Address | Pins (RP2350)     | Speed   |
|-----------|---------|-------------------|---------|
| I2C1      | 0x42    | SDA=GP38, SCL=GP39| 400 kHz |

The Favorite button is a direct GPIO on the RP2350:

| Button   | Pin  | Type    |
|----------|------|---------|
| Favorite | GP25 | GPIO_IN |

## I2C Data Structure (`ui_data_t`)

```c
typedef struct {
    uint16_t pot_adc_values[8];     // raw ADC values from pots
    uint16_t pot_positions[4];      // absolute position 0..1023
    uint8_t  pot_states[4];         // BIT0: fwd, BIT1: bwd, BIT2: fast
    uint16_t d_btns;                // BIT 0-15: D1-D16 step buttons
    uint16_t d_btns_long_press;     // BIT 0-15: D1-D16 long press
    uint8_t  f_btns;                // BIT 0-5: F1, F2, POT1-POT4 press
    uint8_t  f_btns_long_press;
    uint16_t mcl_btns;              // BIT 0-11: MCL buttons (see below)
    uint16_t mcl_btns_long_press;
    int16_t  accelerometer[3];      // 3-axis accelerometer
    uint32_t systicks;              // timestamp
} ui_data_t;
```

## MCL Button Bit Mapping (REV_C)

The `mcl_btns` field is a 12-bit bitmask. Each bit corresponds to:

| Bit | Name         | Physical Label    | Sequencer Function  |
|-----|-------------|-------------------|---------------------|
| 0   | MCL_LEFT    | ← (D-pad left)   | Arrow Left          |
| 1   | MCL_DOWN    | ↓ (D-pad down)   | Arrow Down          |
| 2   | MCL_RIGHT   | → (D-pad right)   | Arrow Right         |
| 3   | MCL_UP      | ↑ (D-pad up)     | Arrow Up            |
| 4   | MCL_FUNC5   | F5 (left of dpad) | Function 5 / A      |
| 5   | MCL_FUNC6   | F6 (right of dpad)| Function 6 / B      |
| 6   | MCL_4_MASTER| Master (white)    | Master mode / X     |
| 7   | MCL_3_SOUND | Sound (blue)      | Sound mode / Y      |
| 8   | MCL_PLAY    | Play/Pause        | Transport Play      |
| 9   | MCL_REC     | Record (red)      | Transport Record    |
| 10  | MCL_S1      | Shift 1           | Shift modifier 1    |
| 11  | MCL_S2      | Shift 2           | Shift modifier 2    |

### Reading a button state

```c
bool is_pressed = (ui_data.mcl_btns >> bit_index) & 1;
```

## D-Buttons (Step Buttons D1-D16)

`d_btns` is a 16-bit field. Bit N = button D(N+1).

```c
bool d1_pressed = (ui_data.d_btns >> 0) & 1;
bool d16_pressed = (ui_data.d_btns >> 15) & 1;
```

## Function Buttons

`f_btns` is a 6-bit field:

| Bit | Label |
|-----|-------|
| 0   | F1    |
| 1   | F2    |
| 2   | POT1 press |
| 3   | POT2 press |
| 4   | POT3 press |
| 5   | POT4 press |

## Potentiometers

4 pots with absolute position (0-1023) and movement states:

```c
uint16_t pos = ui_data.pot_positions[0]; // 0..1023
bool moving_fwd = ui_data.pot_states[0] & 0x01;
bool moving_bwd = ui_data.pot_states[0] & 0x02;
bool moving_fast = ui_data.pot_states[0] & 0x04;
```

## Doom Button Mapping

For the Doom port (feature/doom branch), MCL buttons map to Doom controls:

```
    ┌───────────────────────────────────────────┐
    │         DOOM CONTROL MAPPING              │
    │                                           │
    │    🔴 REC ──────── Automap (TAB)          │
    │    ⚪ PLAY ─────── Pause/Menu (ESC)       │
    │    ⚪ MASTER(X) ── Strafe Modifier (ALT)  │
    │    🔵 SOUND(Y) ── Run/Speed (SHIFT)       │
    │                                           │
    │    F5 (A) ──────── Fire (CTRL)            │
    │    F6 (B) ──────── Use/Open (SPACE)       │
    │                                           │
    │    ↑ ────────────── Forward               │
    │    ↓ ────────────── Backward              │
    │    ← ────────────── Turn Left             │
    │    → ────────────── Turn Right            │
    │                                           │
    │    S1 ───────────── Prev Weapon ([)        │
    │    S2 ───────────── Next Weapon (])        │
    │                                           │
    │    D1-D10 ───────── Weapon select 1-0     │
    │    F1 ───────────── Toggle Automap        │
    │    Favorite ─────── God Mode (IDDQD) ;)   │
    └───────────────────────────────────────────┘
```

| MCL Bit | Button      | Doom Action       | Doom Key        |
|---------|-------------|-------------------|-----------------|
| 0       | ← Left     | Turn Left         | KEY_LEFTARROW   |
| 1       | ↓ Down     | Move Backward     | KEY_DOWNARROW   |
| 2       | → Right    | Turn Right        | KEY_RIGHTARROW  |
| 3       | ↑ Up       | Move Forward      | KEY_UPARROW     |
| 4       | F5 / A      | Fire              | KEY_RCTRL       |
| 5       | F6 / B      | Use / Open Door   | KEY_SPACE       |
| 6       | Master / X  | Strafe Modifier   | KEY_RALT        |
| 7       | Sound / Y   | Run / Speed       | KEY_RSHIFT      |
| 8       | Play / P    | Menu / Pause      | KEY_ESCAPE      |
| 9       | Record / R  | Automap           | KEY_TAB         |
| 10      | S1          | Prev Weapon       | KEY_COMMA       |
| 11      | S2          | Next Weapon       | KEY_PERIOD      |

## NeoPixel LEDs

21 addressable RGB LEDs on GP26:

| Index | Function            |
|-------|---------------------|
| 0     | RP2350 status LED   |
| 1-16  | D-button backlights |
| 17-19 | Function button LEDs|
| 20    | MCL indicator LED   |

LED-to-button mapping arrays:
```c
const uint8_t rgb_led_btn_map[16] = {8, 7, 6, 5, 4, 3, 2, 1, 9, 10, 11, 12, 13, 14, 15, 16};
const uint8_t rgb_led_fbtn_map[3] = {19, 17, 18};
```
