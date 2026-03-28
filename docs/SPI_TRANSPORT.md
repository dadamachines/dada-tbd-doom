# TBD-Pico-Seq3 Complete SPI Transport Analysis

## 1. PIN ASSIGNMENTS (RP2350 → P4 ESP32-S3)

### State API Control SPI (spi0) — Command/Response
**Purpose**: Configuration, plugin commands, JSON queries (blocking)
| Signal | RP2350 GPIO | P4 Pin | Function | Direction |
|--------|------------|--------|----------|-----------|
| MOSI   | GPIO 35    | 23     | TX (cmd data out) | RP→P4 |
| MISO   | GPIO 32    | 22     | RX (response data in) | P4→RP |
| CLK    | GPIO 34    | 21     | SPI clock | RP→P4 |
| CS     | GPIO 33    | 20     | Chip select (active low) | RP→P4 |
| **Speed** | 30 MHz   | —      | —     | — |
| **Physical interface** | DaDa_SPI (Arduino library) | — | — | — |

### Real-Time MIDI SPI (spi1) — Audio/CV Streaming
**Purpose**: Real-time MIDI, CV, triggers (ISR-driven)
| Signal | RP2350 GPIO | P4 Pin | Function | Direction |
|--------|------------|--------|----------|-----------|
| MOSI   | GPIO 31    | 31     | TX (audio/CV data out) | RP→P4 |
| MISO   | GPIO 28    | 29     | RX (response data in) | P4→RP |
| CLK    | GPIO 30    | 28     | SPI clock | RP→P4 |
| CS     | GPIO 29    | 30     | Chip select (active low) | RP→P4 |
| **Speed** | 30 MHz   | —      | —     | — |
| **Sync Pin** | GPIO 27 | 10  | Word Select (I2S WS) | P4→RP (input) |

---

## 2. PROTOCOL STRUCTURES

### State API Request (SpiProtocol.h)
```c
struct p4_spi_request_header {
    uint16_t magic;              // offset 0
    uint8_t request_sequence_counter;  // offset 2
    uint8_t reserved1[5];        // offset 3-7
    uint16_t payload_length;     // offset 8
    uint16_t payload_crc;        // offset 10
    uint32_t reserved2;          // offset 12-15
};  // Total: 16 bytes

struct p4_spi_request2 {
    uint32_t magic;              // 0xCAFEFEED or 0xFEEDC0DE
    uint32_t synth_midi_length;  // MIDI data size (0-256)
    uint8_t synth_midi[256];     // MIDI data payload
    uint32_t sequencer_tempo;    // BPM × 100
    uint32_t sequencer_active_track;
    uint32_t magic2;             // second magic check
};  // Total: 276 bytes
```

### State API Response (SpiProtocol.h)
```c
struct p4_spi_response_header {
    uint16_t magic;              // 0xCAFE
    uint8_t response_sequence_counter;
    uint8_t reserved1[5];
    uint16_t payload_length;
    uint16_t payload_crc;
    uint32_t reserved2;
};  // Total: 16 bytes

struct p4_spi_response2 {
    uint32_t magic;              // 0xCAFEFEED
    uint32_t usb_device_midi_length;
    uint8_t usb_device_midi[256];
    uint8_t input_waveform[64];
    uint8_t output_waveform[64];
    uint8_t link_data[64];
    uint32_t led_color;
    uint32_t webui_update_counter;
    uint32_t magic2;
};  // Total: 464 bytes
```

### Fingerprint & Handshake
- **Magic bytes**: 0xCA, 0xFE (fingerprint, checked at offset 0-1)
- **Request type**: 1 byte at offset 2
- **Payload length**: 4 bytes at offset 3
- **Payload**: Variable, max 2048 - 7 = 2041 bytes per transfer
- **CRC**: Simple checksum: `sum = 42 + Σ payload[i]`

---

## 3. STATE API (SpiAPI) — COMMAND TRANSPORT

### Class SpiAPI Pinning (PINMAP.md shows dual versions)
```cpp
// REV_B (no RDY pin):
DaDa_SPI cmd_api_spi {spi0, 33, 35, 32, 34, 30000000};
// Parameters: (spi_inst, mosi, miso, clk, cs, speed)

// REV_C (with RDY/ready pin):
DaDa_SPI cmd_api_spi {spi0, 33, 35, 32, 34, 18, 30000000};
// GPIO 18 = RDY signal from P4 (ready status)
```

### Initialization (SpiAPI.cpp:Init)
```cpp
void SpiAPI::Init() {
    out_buf[0] = 0xCA;
    out_buf[1] = 0xFE;  // Fingerprint set once
}
```

### Main Send Function (SpiAPI.cpp:56)
```cpp
bool SpiAPI::transmitData(const std::string &data, RequestType_t reqType) {
    uint32_t len = data.length();
    const char* str = data.c_str();
    
    *request_type = reqType;
    uint32_t *lengthField = (uint32_t*)(out_buf + 3);
    uint32_t bytes_sent = 0;
    
    while (len > 0) {
        *lengthField = len;
        *request_type = reqType;
        
        // Calculate chunk size: max 2048 - 7 header bytes = 2041
        bytes_to_send = (len > 2048 - 7) ? (2048 - 7) : len;
        
        // Copy string chunk to buffer starting at offset 7
        memcpy(out_buf + 7, str + bytes_sent, bytes_to_send);
        
        len -= bytes_to_send;
        bytes_sent += bytes_to_send;
        
        // Blocking SPI transfer with delay
        cmd_api_spi.TransferBlockingDelayed(out_buf, in_buf, 2048);
        
        // Verify response fingerprint & ACK
        if (in_buf[0] != 0xCA || in_buf[1] != 0xFE) return false;
        if (in_buf[2] != reqType) return false;
    }
    return true;
}
```

### Receive Response (SpiAPI.cpp:72)
```cpp
bool SpiAPI::receiveData(std::string& response, RequestType_t request) {
    cmd_api_spi.TransferBlockingDelayed(out_buf, in_buf, 2048);
    
    // Fingerprint check
    if (in_buf[0] != 0xCA || in_buf[1] != 0xFE) return false;
    if (in_buf[2] != request) return false;
    
    // Read length from offset 3
    const uint32_t* resLength = (uint32_t*)&in_buf[3];
    const uint32_t totalResponseLength = *resLength;
    
    // First chunk (payload starts at offset 7)
    uint32_t bytes_received = (*resLength > 2041) ? 2041 : *resLength;
    uint32_t bytes_to_be_received = *resLength - bytes_received;
    response.append((char*)&in_buf[7], bytes_received);
    
    // Additional chunks if needed
    while (bytes_to_be_received > 0) {
        cmd_api_spi.TransferBlockingDelayed(out_buf, in_buf, 2048);
        
        // Verify each chunk
        if (in_buf[0] != 0xCA || in_buf[1] != 0xFE) return false;
        
        bytes_received = (*resLength > 2041) ? 2041 : *resLength;
        response.append((char*)&in_buf[7], bytes_received);
        bytes_to_be_received -= bytes_received;
    }
    
    return (response.size() == totalResponseLength);
}
```

### Synchronization with P4 (REV_C only)
```cpp
#if REV_C
// Block until P4 is ready
cmd_api_spi.WaitUntilP4IsReady();  // Checks GPIO 18 or RDY signal

// Typical post-command wait
delay(10);  // 10 ms after SetActivePlugin, etc.
#endif

#if REV_B
// For REV_B boards without RDY pin, use fixed delays
delay(1000);  // 1 second for LoadPreset, SavePreset, SetConfiguration, etc.
delay(2000);  // 2 seconds for SaveFavorite, LoadFavorite
#endif
```

### Per-Request Type Handshake
| Cmd Type | Method | Wait After Send | Notes |
|----------|--------|-----------------|-------|
| GetPlugins | send() + receiveData() | None | JSON response |
| SetActivePlugin | send() | WaitSpiAPIReadyForCmd() | REV_C only |
| LoadPreset | send() | 1-2s delay | REV_B; instant (REV_C) |
| GetActivePlugin | send() + receiveData() | None | JSON response |
| SetPluginParam | send() | WaitSpiAPIReadyForCmd() | REV_C only |

---

## 4. REAL-TIME MIDI SPI (spi1) — NOT IMPLEMENTED IN SEQ3

**Note**: The examples/main.cpp uses `MidiP4_2` which queues MIDI data into buffers
but **does NOT actually transmit over Spi1** in the reference code shown.

**Structure** (from MidiP4.h):
```c
struct p4_spi_request {  // For real-time link (not fully implemented)
    uint32_t magic;
    uint32_t synth_midi_length;
    uint8_t synth_midi[256];  // MIDI data
    uint32_t sequencer_tempo;
    uint32_t sequencer_active_track;
    uint32_t magic2;
};
```

**Known from dada-tbd-doom** (for reference):
- GPIO 28-31 for SPI1 (alternate implementation)
- 30 MHz clock
- Manual CS on GPIO 29
- RDY input on GPIO 27 (word-clock sync)
- DMA-driven double-buffering for continuous streaming
- Word-clock ISR @ 1.378 kHz paces frame transmission
- 512-byte request per frame OR 512-byte response

---

## 5. BOOT SEQUENCE (examples/main.cpp:setup)

### Phase 1: Initialization (order matters!)
```
1. transmitter.begin(115200)      // PIO UART on GPIO 20
2. mutex_init()                   // For song/sequencer state
3. malloc() buffers               // Song, save, track buffers
4. queue_init() × 4               // queues for MIDI, UI, updates, background
5. spi_api.Init()                 // Set fingerprint = 0xCAFE
   → Sets out_buf[0]=0xCA, out_buf[1]=0xFE
6. ui_display.Init()              // OLED display on PIO SPI
7. ui_inputs.Init()               // Button/knob inputs
8. ui_leds.Init()                 // Neopixel LED
9. _sdcard.Init()                 // SD card on SDIO
```

### Phase 2: P4 Synchronization
```
10. sleep_ms(1000)
11. spi_api.Reboot()              // Send REBOOT cmd to P4
    → RequestType = 0x13 (Reboot)
    → sleep_ms(10000) in Reboot()
    → P4 reboots and re-enumates
    
12. sleep_ms(1000)
13. spi_api.AnnounceApp("Groovebox", 0x03)
    → RequestType = 0xAB (AnnounceApp)
    → flags = 0x03 (plugin_lock | redirect_samples)
    → Informs P4 of RP2350 app capabilities
```

### Phase 3: Boot Screen
```
14. bootScreen = new BootScreen()
15. screenRouter.setRoot(bootScreen)
    → Transitions to boot screen UI
```

### Phase 4: Sequencer Initialization (boot_into_sequencer)
```
16. sequi.init()
17. sequi.loadOrInitConfig()
18. mutexes acquired
19. initializeAllPresets()
20. initializeSongAndTracks()
21. fetchAndInitTrackDefaults()    // Query P4: GetTrackDefaultPresets (0xA5)
    → Response: JSON with per-track preset IDs
22. GetKitIndexJSON()              // Query available kits
23. SetActiveSampleKit(kitindex)   // SPI cmd 0x18
24. GetSampleBankIndexJSON()       // Query available banks
25. For each track: initializeMacroPresetOnTrack()
    → GetMacroSoundPresetList(trackIndex)  // cmd 0xA0
    → LoadTrackSoundPreset(trackIndex, presetId)  // cmd 0xA4
```

---

## 6. FRAME PACKING & MULTI-FRAME TRANSFERS

### Buffer Layout (out_buf, 2048 bytes)
```
Offset   Field              Size    Purpose
────────────────────────────────────────────────────
0-1      Fingerprint        2       0xCA, 0xFE (always)
2        Request Type       1       0x01..0xAB (command ID)
3-6      Payload Length     4       Total bytes following (uint32_t LE)
7-2047   Payload Data       2041    JSON cstring, binary, etc.
```

### Multi-Frame Protocol
**For large payloads (e.g., JSON > 2041 bytes)**:
1. **Frame 1**: Send first 2041 bytes + length header (total payload length)
2. **P4 Response**: 0xCA, 0xFE, [ACK type], [length], [partial response]
3. **Frame 2+**: Send next 2041 bytes (fingerprint + type + length + chunk)
4. **Repeat** until all bytes sent

**CRC Validation**:
```cpp
uint16_t calcPayloadCrc(uint8_t *data, uint16_t length) {
    uint16_t sum = 42;  // Initial value
    for(uint16_t i = 0; i < length; i++) {
        sum += data[i];
    }
    return sum;
}
```

---

## 7. SYNCHRONIZATION & WAIT MECHANISMS

### RDY Signal (REV_C only)
- **GPIO 18** (RP2350) ← P4 "ready" status
- Indicates P4 has finished processing last command
- Methods:
  - `WaitUntilP4IsReady()` — blocks on GPIO 18 high
  - `GetP4Ready()` — non-blocking status check

### Blocking Delays (REV_B)
- **1 second**: LoadPreset, SavePreset, SetConfiguration, SetPluginParam
- **2 seconds**: SaveFavorite, LoadFavorite, SetPluginParamsJSON

### TransferBlockingDelayed()
- DaDa_SPI library method
- Performs full-duplex SPI transfer: pads ignored bytes, reads response simultaneously
- 15 µs post-transfer delay (documented in dada-tbd-doom/src/p4_control_link.c)

---

## 8. REQUEST TYPES (SpiAPI.h)

**High-Frequency Commands** (return immediately or with minimal wait):
- 0x01: GetPlugins
- 0x02: GetActivePlugin
- 0x03: GetActivePluginParams
- 0x08: GetPresets
- 0x09: GetPresetData
- 0x0D: GetAllFavorites
- 0x10: GetConfiguration
- 0x12: GetIOCapabilities
- 0x16: GetSampleRomDescriptor
- 0x19: GetFirmwareInfo

**Long-Running Commands** (require RDY wait or fixed delay):
- 0x04: SetActivePlugin → WaitSpiAPIReadyForCmd()
- 0x05: SetPluginParam → WaitSpiAPIReadyForCmd()
- 0x06: SetPluginParamCV → WaitSpiAPIReadyForCmd()
- 0x07: SetPluginParamTRIG → WaitSpiAPIReadyForCmd()
- 0x0A: SetPresetData → transmitData() + WaitSpiAPIReadyForCmd()
- 0x0B: LoadPreset → delay(1000-2000)
- 0x0C: SavePreset → delay(1000-2000)
- 0x0E: SaveFavorite → transmitData() + delay(1000-2000)
- 0x0F: LoadFavorite → delay(1000-2000)
- 0x11: SetConfiguration → transmitData() + WaitSpiAPIReadyForCmd()
- 0x13: Reboot → delay(10000)
- 0x14: SetPluginParamsJSON → transmitData() + WaitSpiAPIReadyForCmd()
- 0x15: RebootToOTA1 → delay(1000)
- 0x17: SetActiveWaveTableBank → WaitSpiAPIReadyForCmd()
- 0x18: SetActiveSampleKit → cmd_api_spi.WaitUntilP4IsReady()
- 0x20: SetAbletonLinkTempo → WaitSpiAPIReadyForCmd()
- 0x21: SetAbletonLinkStartStop → WaitSpiAPIReadyForCmd()
- 0xA3: ActivateTrackMachine → (no wait shown)
- 0xA4: LoadTrackSoundPreset → WaitSpiAPIReadyForCmd()
- 0xA9: PutSamplePresetJSON → transmitData()
- 0xAA: LoadTrackMacroDefinition → (no wait shown)
- 0xAB: AnnounceApp → send() only

---

## 9. CLOCK SOURCE & TIMING

### Word-Clock Synchronization (P4 I2S WS → RP2350)
- **P4 GPIO 10** (I2S_WS) → **RP2350 GPIO 27**
- Codec word-clock: 44.1 kHz (stereo, 16-bit)
- **Divide by 32**: Frame rate = 44.1 kHz / 32 = ~1.378 kHz
- Used by dada-tbd-doom for ISR-driven SPI pacing (not SEQ3)

### SPI Clock
- **30 MHz** for both spi0 and spi1
- **Mode 3** (CPOL=1, CPHA=1) inferred from context
- Full-duplex simultaneously TX and RX

---

## 10. SUMMARY: CRITICAL IMPLEMENTATION POINTS

1. **Two separate SPI buses**: spi0 (state API) and spi1 (real-time—not used in SEQ3)
2. **Fingerprint-first protocol**: Every message starts 0xCA, 0xFE
3. **Blocking transfers**: SpiAPI uses fully synchronous request-response
4. **Multi-frame chunking**: Payloads > 2041 bytes sent in multiple 2048-byte transfers
5. **P4 ready handshake**: REV_C checks GPIO 18; REV_B uses fixed 1-2s delays
6. **Sequence counter**: Increments 100-199, wraps at 100+100 = 200
7. **CRC**: Simple sum-based, not cryptographic
8. **Boot order**: Reboot P4 → AnnounceApp → load song → fetch defaults from P4 SD
9. **Pin conflicts**: None; spi0 (GPIO 32-35) separate from spi1 (GPIO 28-31)
10. **DMA**: Not used in SpiAPI; dma only in dada-tbd-doom's p4_spi_transport (real-time link)
