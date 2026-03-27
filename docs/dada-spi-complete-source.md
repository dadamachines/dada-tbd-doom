# DaDa_SPI Library - Complete Analysis

## Source Location
- **Library**: `/Users/jlo/Documents/GitHub/tbd-pico-seq3/.pio/libdeps/pi2350/DaDa_SPI/`
- **From**: `https://github.com/ctag-fh-kiel/DaDa_SPI.git` (versions: v1.0.3, v1.0.5)
- **Single header-only library**: `DaDa_SPI.h` (implementation inline in header)

## Constructor Details

### Signature
```cpp
DaDa_SPI(spi_inst_t* spi_port, uint cs, uint mosi, uint miso, uint clk, uint handshake_pin, uint speed)
```

### Parameters
1. `spi_inst_t* spi_port` — SPI instance (spi0, spi1)
2. `uint cs` — Chip select GPIO pin
3. `uint mosi` — Master Out Slave In GPIO pin
4. `uint miso` — Master In Slave Out GPIO pin
5. `uint clk` — Clock GPIO pin
6. `uint handshake_pin` — Ready signal GPIO pin (0=busy, 1=ready)
7. `uint speed` — SPI speed in Hz

### Instantiation Examples
**Command API (tbd-pico-seq3)**:
```cpp
DaDa_SPI cmd_api_spi {spi0, 33, 35, 32, 34, 18, 30000000};  // REV_C with handshake pin 18
DaDa_SPI cmd_api_spi {spi0, 33, 35, 32, 34, 30000000};      // REV_B without explicit handshake (5 params)
```

**Real-time Audio (dada-tbd-doom reference)**:
```cpp
DaDa_SPI real_time_spi {spi1, 29, 31, 28, 30, 22, 30000000};  // spi1, 30 MHz, handshake pin 22
```

## Constructor Implementation (Complete)

### DMA Channel Setup
```cpp
// Claim two unused DMA channels
dma_tx_spi = dma_claim_unused_channel(true);
dma_rx_spi = dma_claim_unused_channel(true);
```

### TX DMA Config
```cpp
ctx_spi = dma_channel_get_default_config(dma_tx_spi);
channel_config_set_transfer_data_size(&ctx_spi, DMA_SIZE_8);          // 8-bit transfers
channel_config_set_dreq(&ctx_spi, spi_get_dreq(_spi_port, true));    // TX DREQ
channel_config_set_read_increment(&ctx_spi, true);                    // Increment read addr (source buffer)
channel_config_set_write_increment(&ctx_spi, false);                  // Don't increment write addr (SPI DR)
```

### RX DMA Config
```cpp
crx_spi = dma_channel_get_default_config(dma_rx_spi);
channel_config_set_transfer_data_size(&crx_spi, DMA_SIZE_8);          // 8-bit transfers
channel_config_set_dreq(&crx_spi, spi_get_dreq(_spi_port, false));   // RX DREQ
channel_config_set_read_increment(&crx_spi, false);                   // Don't increment read addr (SPI DR)
channel_config_set_write_increment(&crx_spi, true);                   // Increment write addr (dest buffer)
```

### GPIO Setup
```cpp
// Handshake pin: input, pull-down (P4 drives high when ready)
gpio_init(_handshake_pin);
gpio_set_dir(_handshake_pin, GPIO_IN);
gpio_pull_down(_handshake_pin);
```

### SPI Setup
```cpp
spi_init(_spi_port, _spi_speed);
gpio_set_function(_spi_miso, GPIO_FUNC_SPI);    // Set to SPI function
gpio_set_function(_spi_sclk, GPIO_FUNC_SPI);
gpio_set_function(_spi_mosi, GPIO_FUNC_SPI);
gpio_set_function(_spi_cs, GPIO_FUNC_SPI);      // Hardware-managed CS

// CPOL=1, CPHA=1, MSB-first
spi_set_format(_spi_port, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);
```

## TransferBlockingDelayed() Method - Exact Sequence

### Signature
```cpp
void TransferBlockingDelayed(uint8_t* tx_buf, uint8_t* rx_buf, uint len, uint delay_us=15)
```

### Execution Order
```
1. WaitUntilDMADoneBlocking()     // Block until previous DMA done
2. [COMMENTED OUT: pre-DMA delay doesn't work]
3. WaitUntilP4IsReady()            // Poll handshake pin until high
4. StartDMA(tx_buf, rx_buf, len)  // Configure and start both TX/RX DMA
5. WaitUntilDMADoneBlocking()     // Block until transfer complete
6. if (delay_us > 0)
     busy_wait_us_32(delay_us)     // Post-transfer delay (default 15µs)
```

## StartDMA() Method - Detailed Implementation

```cpp
void StartDMA(uint8_t* tx_buf, uint8_t* rx_buf, uint len){
    // Configure TX DMA channel
    dma_channel_configure(dma_tx_spi, &ctx_spi,
                          &spi_get_hw(_spi_port)->dr,    // Write address: SPI data register
                          tx_buf,                          // Read address: TX buffer
                          len,                             // Element count
                          false);                          // Don't start yet

    // Configure RX DMA channel
    dma_channel_configure(dma_rx_spi, &crx_spi,
                          rx_buf,                          // Write address: RX buffer
                          &spi_get_hw(_spi_port)->dr,    // Read address: SPI data register
                          len,
                          false);

    // Start both channels simultaneously
    dma_start_channel_mask((1u << dma_tx_spi) | (1u << dma_rx_spi));
}
```

## Helper Methods

### WaitUntilP4IsReady()
```cpp
void WaitUntilP4IsReady(){
    while(!gpio_get(_handshake_pin)) tight_loop_contents();
}
```
Polls handshake pin in tight loop until P4 signals ready (high).

### GetP4Ready()
```cpp
bool GetP4Ready(){
    return gpio_get(_handshake_pin);
}
```
Non-blocking check of handshake pin state.

### WaitUntilDMADoneBlocking()
```cpp
void WaitUntilDMADoneBlocking(){
    while(IsBusy()) tight_loop_contents();
}
```

### IsBusy()
```cpp
bool IsBusy(){
    return dma_channel_is_busy(dma_tx_spi) || dma_channel_is_busy(dma_rx_spi);
}
```

## Includes
```cpp
#include <hardware/dma.h>    // DMA functions (dma_claim_unused_channel, dma_channel_configure, etc)
#include <hardware/gpio.h>   // GPIO functions (gpio_init, gpio_get, gpio_set_function, etc)
#include <hardware/spi.h>    // SPI functions (spi_init, spi_get_hw, spi_get_dreq, spi_set_format)
#include <Arduino.h>         // Arduino API (busy_wait_us_32)
```

## CS Handling
- **Method**: Hardware-managed GPIO_FUNC_SPI
- **Pin**: Passed as `cs` parameter to constructor
- **Behavior**: SPI peripheral asserts CS for entire transfer (CPOL=1, CPHA=1)
- **Comment in code**: "this mode allows to assert CS for entire transfer by spi hw peripheral; only mode supported in this lib"

## Real-Time Characteristics
- Blocking DMA transfers only
- No interrupt-driven transfer support
- Fixed ~15µs post-transfer delay (tuned empirically)
- Handshake protocol prevents clobbering P4 SPI input buffer
- Idempotent delay (works before/after transfer but NOT between ready-check and DMA start)

## tbd-pico-seq3 Usage (SpiAPI.cpp)

```cpp
// In SpiAPI class (declared in .h):
DaDa_SPI cmd_api_spi {spi0, 33, 35, 32, 34, 18, 30000000};  // REV_C

// Called in transmitData():
cmd_api_spi.TransferBlockingDelayed(out_buf, in_buf, 2048);

// Called in receiveData():
cmd_api_spi.TransferBlockingDelayed(out_buf, in_buf, 2048);

// Called in send():
cmd_api_spi.TransferBlockingDelayed(out_buf, in_buf, 2048);

// REV_C also uses handshake pre-check:
cmd_api_spi.WaitUntilP4IsReady();  // In SetActivePlugin, LoadPreset, SavePreset
```

## Key Design Points
1. **Blocking only** — no non-blocking option (use StartDMA directly for async)
2. **DMA setup deferred** — channels allocated in ctor, configured on each transfer
3. **Tight-loop polling** — busy-waits for handshake and DMA completion
4. **Fixed 15µs post-delay** — empirically determined, only works AFTER DMA completes
5. **Symmetric DMA** — TX and RX must run simultaneously for full-duplex
6. **SPI mode fixed** — CPOL=1, CPHA=1, 8-bit MSB-first (no other modes supported)
