#pragma once

#include <atomic>
#include <string>
#include <ArduinoJson.h>
#include <Midi.h>

// defines for the CTAG TBD UI board with STM
#include <Wire.h>
#define I2C_SLAVE_ADDR 0x42
#define I2C_SDA 38
#define I2C_SCL 39

// defines for the OLED display, PIO SPI
#include <SoftwareSPI.h>
#include <Adafruit_GFX.h>
#include "DaDa_SSD1309.h"
#define OLED_MOSI 15
#define OLED_SCLK 14
#define OLED_DC 12
#define OLED_CS 13
#define OLED_RST 16

// neopixels for UI
#include <Adafruit_NeoPixel.h>
#define LED_COUNT 21
#define LED_PIN 26

typedef struct{
    uint16_t pot_adc_values[8]; // raw adc values
    uint16_t pot_positions[4]; // absolute position 0..1023
    uint8_t pot_states[4]; // BIT0: fwd, BIT1: bwd, BIT2: fast
    // 16-bit
    uint16_t d_btns; // BIT0-15: D1-D16
    uint16_t d_btns_long_press; // BIT0-15: D1-D16
    // function buttons are (0: F1, 1: F2, 2: POT1 (left), 3: POT2, 4: POT3, 5: POT4 (right))
    // 6-bit
    uint8_t f_btns;
    uint8_t f_btns_long_press;
    // mcl buttons are (0: MCL_LEFT, 1: MCL_DOWN, 2: MCL_RIGHT, 3: MCL_UP, 4: MCL_A, 5: MCL_B, 6: MCL_X, 7: MCL_Y, 8: MCL_P, 9: MCL_R, 10: MCL_S1, 11: MCL_S2)
    // 12-bit
    uint16_t mcl_btns;
    uint16_t mcl_btns_long_press;
    int16_t accelerometer[3];
    uint32_t systicks; // timestamp
} ui_data_t;

class Ui{
    Midi &midi;
    ui_data_t ui_data;
    uint32_t current_ui_data = 0; // current ui data index
    SoftwareSPI softSPI{SoftwareSPI(OLED_SCLK, OLED_DC, OLED_MOSI)};
    DaDa_SSD1309 display{DaDa_SSD1309(128, 64, &softSPI, OLED_DC, OLED_RST, OLED_CS)};
    Adafruit_NeoPixel strip = Adafruit_NeoPixel(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);
    unsigned long previousMillis = 0;
    const uint8_t rgb_led_rp2350 = 0;
    const uint8_t rgb_led_btn_map[16] = {8, 7, 6, 5, 4, 3, 2, 1, 9, 10, 11, 12, 13, 14, 15, 16};
    const uint8_t rgb_led_fbtn_map[3] = {19, 17, 18};
    uint32_t ledStatus = 0;
    bool p4Ready{false}; // P4 ready indicator
    bool resetRequested{false}; // reset request indicator
    bool sdInitialized{false}; // SD card initialized indicator
    bool doomAudioInitialized{false}; // one-shot init for doom audio plugin

    void displayString(const std::string& s);
    void displayStringWait1s(const std::string& s);

public:
    Ui(Midi &midi) : midi{midi} {}
    void Init();
    void InitHardware();
    void InitDisplay();
    void InitLeds();
    void InitSDCard();
    void Poll();
    void Update();
    bool UpdateUIInputs();
    void UpdateUIInputsBlocking();

    // examples
    void LoadDrumRackAndMapNoteOnsExample();
    void RealTimeCVTrigAPIExample();
    void BootIntoOTA1(); // can be used to reboot the P4 into sd-card mode (if available on the P4=
    void GetAndDisplaySampleRomDescriptor_SetToBank1();
    void GetP4FirmwareInfo();

    // tests
    void RunUITests();
    void RunSpiAPITests();
    void RunPSRAMTests();
    void RunSDCardTests();

    ui_data_t CopyUiData(){
        return ui_data;
    }
};
