#include "Midi.h"
#include "MidiParser.h"
#include "MidiRunningStatusExpander.h"
#include <SerialPIO.h>

static MidiParser midiparser; // MIDI handling
static MidiRunningStatusExpander midi_exp_uart0; // MIDI running status expander
static MidiRunningStatusExpander midi_exp_uart1; // MIDI running status expander

#define USBA_PWR_ENA_GPIO 10
#define USBA_SEL_GPIO 11

// real-time SPI data transfer buffer size
// tested with 2048 bytes, spi operating at 30MHz
// max. is about 32/44100 * 30000000 / 8 = 2721 bytes
// data rate with 2048 bytes approx. 44100/32 * 2048 / 1024 / 1024 = 2.7 MB/s
// 1024 seems to be a safe margin for a transfer to be completed in time
// 2048 works as well
// this value must correspond to the SPI_BUFFER_LEN in the P4 firmware (rp2350_spi_stream.cpp)
#define SPI_BUFFER_LEN 1024

#define BUF_OFFSET_LED 2 // two bytes for led status
#define BUF_OFFSET_ABLETON_LINK_DATA (BUF_OFFSET_LED + 4) // 4 bytes for led data
#define BUF_OFFSET_MIDI_LENGTH (BUF_OFFSET_ABLETON_LINK_DATA + sizeof(link_session_data_t)) // sizeof(link_session_data_t) bytes for link data
#define BUF_OFFSET_MIDI_DATA (BUF_OFFSET_MIDI_LENGTH + 4) // 4 bytes for midi length

// sync codec 44100Hz
#define WS_PIN 27

#define LED_GREEN 25

static bool led_state = false;


// transfer structure is
// byte 0, 1 -> 0xCA 0xFE (fingerprint)
typedef struct{
    uint8_t out_buf[SPI_BUFFER_LEN], in_buf[SPI_BUFFER_LEN]; // actual buffers
} spi_trans_t;

static spi_trans_t spi_trans[2];
static uint32_t current_trans = 0;

#include "Adafruit_TinyUSB.h"
// Add USB MIDI Host support to Adafruit_TinyUSB
#include "usb_midi_host.h"

// USB Host object
static Adafruit_USBH_Host USBHost;

// holding device descriptor
static tusb_desc_device_t desc_device;

// holding the device address of the MIDI device
static uint8_t midi_dev_addr = 0;

#include <atomic>
std::atomic<uint32_t> ws_sync_counter{0}; // counter for word clock sync

static void ws_sync_cb(){
    // sync to word clock of codec i2s @ 44100Hz
    // divider 32 is block size of TBD
    static uint32_t cnt = 0;
    cnt++;
    if (cnt % 32 == 0){
        ws_sync_counter++;
    }
}

//--------------------------------------------------------------------+
// TinyUSB Host callbacks
//--------------------------------------------------------------------+

// Invoked when device is mounted (configured)
void tuh_mount_cb(uint8_t daddr){
    // Get Device Descriptor
    tuh_descriptor_get_device(daddr, &desc_device, 18, NULL, 0);
}

/// Invoked when device is unmounted (bus reset/unplugged)
void tuh_umount_cb(uint8_t daddr){
}

//--------------------------------------------------------------------+
// TinyUSB Callbacks
//--------------------------------------------------------------------+

// Invoked when device with hid interface is mounted
// Report descriptor is also available for use. tuh_hid_parse_report_descriptor()
// can be used to parse common/simple enough descriptor.
// Note: if report descriptor length > CFG_TUH_ENUMERATION_BUFSIZE, it will be skipped
// therefore report_desc = NULL, desc_len = 0
void tuh_midi_mount_cb(uint8_t dev_addr, uint8_t in_ep, uint8_t out_ep, uint8_t num_cables_rx, uint16_t num_cables_tx){
    if (midi_dev_addr == 0){
        // then no MIDI device is currently connected
        midi_dev_addr = dev_addr;
    }
}

// Invoked when device with hid interface is un-mounted
void tuh_midi_umount_cb(uint8_t dev_addr, uint8_t instance){
    if (dev_addr == midi_dev_addr){
        midi_dev_addr = 0;
    }
}

void tuh_midi_rx_cb(uint8_t dev_addr, uint32_t num_packets){
    if (midi_dev_addr == dev_addr){
        if (num_packets != 0){
            uint8_t cable_num;
            uint8_t buffer[48];
            while (1){
                uint32_t bytes_read = tuh_midi_stream_read(dev_addr, &cable_num, buffer, sizeof(buffer));
                if (bytes_read == 0) return;
                // blink a bit to indicate that we received a MIDI message
                digitalWrite(LED_GREEN, led_state);
                led_state = !led_state;
                midiparser.QueueData(buffer, bytes_read); // queue the data for processing
            }
        }
    }
}

void Midi::Init(){
    // WS sync to codec
    pinMode(WS_PIN, INPUT_PULLDOWN); // Configure button pin with pull-down resistor
    attachInterrupt(digitalPinToInterrupt(WS_PIN), ws_sync_cb, FALLING);

    // indicator LED
    pinMode(LED_GREEN, OUTPUT);

    // reset sync counter
    ws_sync_counter = 0;
    p4Alive = false;
    while (ws_sync_counter < 100) delay(10); // wait for p4 codec to come alive
    p4Alive = true;

    midiparser.Init(); // Initialize MIDI handling

    // Optionally, configure the buffer sizes here
    // The commented out code shows the default values
    // tuh_midih_define_limits(64, 64, 16);
    USBHost.begin(0); // 0 means use native RP2040 host

    // USB Host init
    pinMode(USBA_PWR_ENA_GPIO, OUTPUT);
    pinMode(USBA_SEL_GPIO, OUTPUT);
    digitalWrite(USBA_PWR_ENA_GPIO, true); // enable USB power
    digitalWrite(USBA_SEL_GPIO, true); // select USB A port


    // UARTS / MIDI
    // the mapping uart0 = Serial1 and uart1 = Serial2 is fixed in Arduino
    // https://arduino-pico.readthedocs.io/en/latest/serial.html
    // uart0 = Serial1 = TBD MIDI IN/OUT2
    Serial1.setTX(44); // set TX pin for first UART
    Serial1.setRX(45); // set RX pin for first UART
    Serial1.begin(31250); // MIDI baud rate
    // uart1 = Serial2 = TBD MIDI IN/OUT1
    Serial2.setTX(36); // set TX pin for second UART
    Serial2.setRX(37); // set RX pin for second UART
    Serial2.begin(31250); // MIDI baud rate for second UART


    // SPI data init
    // use double buffering
    // uart1 = Serial2 = TBD IN/OUT1
    spi_trans[0].out_buf[0] = 0xCA; // fingerprint
    spi_trans[0].out_buf[1] = 0xFE; // fingerprint
    spi_trans[1].out_buf[0] = 0xCA; // fingerprint
    spi_trans[1].out_buf[1] = 0xFE; // fingerprint
    current_trans = 0;

    // init real-time state buffer
    memset(real_time_state_buffer, 0, N_CVS_TBD * 4 + N_TRIGS_TBD); // clear the buffer
    mutex_init(&real_time_mutex); // initialize the mutex for real-time state buffer
    mutex_init(&ableton_link_data_mutex); // initialize the mutex for real-time state buffer
}

void Midi::Update(){
    // check if p4 is still alive
    static unsigned long time {0};
    unsigned long elapsed = millis() - time;
    if (elapsed > 100 && ws_sync_counter == 0){
        // if we have not received a sync signal for 100ms, then we assume that the P4 is not alive
        p4Alive = false;
    } else {
        p4Alive = true; // P4 is alive
    }

    // update midi host
    bool connected = midi_dev_addr != 0 && tuh_midi_configured(midi_dev_addr);
    USBHost.task();

    // update uarts
    // get data from UARTS, expand message, sometime single bytes are received, sometimes they may be running status
    while (Serial1.available() > 0){
        uint8_t val = Serial1.read();
        //Serial1.write(val); // midi thru
        MidiRunningStatusExpander::FeedResult res = midi_exp_uart0.Feed(val);
        if (res == MidiRunningStatusExpander::FeedResult::MessageComplete){
            int len;
            const uint8_t* msg = midi_exp_uart0.GetMessage(len);
            midiparser.QueueData((uint8_t*)msg, len);
        }
    }
    while (Serial2.available() > 0){
        MidiRunningStatusExpander::FeedResult res = midi_exp_uart1.Feed(Serial2.read());
        if (res == MidiRunningStatusExpander::FeedResult::MessageComplete){
            int len;
            const uint8_t* msg = midi_exp_uart1.GetMessage(len);
            midiparser.QueueData((uint8_t*)msg, len);
        }
    }

    // prepare real-time SPI transfer
    if (ws_sync_counter > 0){ // 725,62us have passed -> 44100Hz / 32 = 1378,125Hz
        // get time of last ws sync to detect if p4 is alive
        time = millis();

        // check if previous real-time control DMA is done
        real_time_spi.WaitUntilDMADoneBlocking();
        // check if P4 is ready to receive data
        real_time_spi.WaitUntilP4IsReady();

        // schedule next DMA transfer
        real_time_spi.StartDMA(spi_trans[current_trans].out_buf, spi_trans[current_trans].in_buf, SPI_BUFFER_LEN);

        // swap buffers
        current_trans ^= 0x1;

        // get forwarded data from p4 usb device in, send through regular spi transaction
        if (spi_trans[current_trans].in_buf[0] == 0xCA && spi_trans[current_trans].in_buf[1] == 0xFE){
            // fingerprint matches, we have a valid transfer
            // update the LED status from the SPI transfer
            uint32_t *led = (uint32_t *) &spi_trans[current_trans].in_buf[BUF_OFFSET_LED];
            ledStatus = *led; // update led status from SPI transfer
            link_session_data_t *link_data_ = (link_session_data_t *) &spi_trans[current_trans].in_buf[BUF_OFFSET_ABLETON_LINK_DATA];
            if (mutex_try_enter(&ableton_link_data_mutex, nullptr)){
                memcpy(&link_data, link_data_, sizeof(link_session_data_t));
                mutex_exit(&ableton_link_data_mutex);
            };
            // see if we have USB device midi data from p4?
            uint32_t *midi_len = (uint32_t*) &spi_trans[current_trans].in_buf[BUF_OFFSET_MIDI_LENGTH];
            uint8_t *midi_data = (uint8_t*) &spi_trans[current_trans].in_buf[BUF_OFFSET_MIDI_DATA];
            midiparser.QueueData(midi_data, *midi_len);
            // forward to UARTS
            if (*midi_len > 0){
                Serial1.write(midi_data, *midi_len);
                Serial2.write(midi_data, *midi_len); // from p4 usb device midi in
            }
        }
        // check if we should use legacy midi parser
        if (!bypassLegacyMidiParser){
            // if we have a word clock sync /32 = one block size, then we can update the MIDI parser
            // replace the midi parser for your own implementation if you want to use a different MIDI parser or none at all!
            midiparser.Update(spi_trans[current_trans].out_buf + 2); // skip fingerprint bytes
            // ATTENTION:
            // old midiparser works with N_CV CVs and N_TRIG TRIGs, TBD fw expects as defined in Midi.h N_CVS_TBD CVs and N_TRIG_TBD TRIGs, so we have to rearrange the data for the transmission
            // move the TRIGs to the correct location of the buffer after the CVs from the old midi parser
            memcpy(&spi_trans[current_trans].out_buf[2 + N_CVS_TBD * 4], &spi_trans[current_trans].out_buf[2 + N_CVS * 4], N_TRIGS);
            // set new CVs to zeros, HERE YOU COULD ADD YOUR OWN CONTROLS
            // offset in outbuf for 240 CVs is 2 (for fingerprint)
            // offset in outbuf for 90 CVs is 2 (fingerprint) + 240 * 4 = 962
            memset(&spi_trans[current_trans].out_buf[2 + N_CVS * 4], 0, (N_CVS_TBD - N_CVS) * 4); // set the rest of the CVs to zero, we only use 90 CVs from the old midi parser
        }else{
            // use real-time state buffer directly, bypass the legacy midi parser
            // try to enter the mutex
            if (!mutex_try_enter(&real_time_mutex, nullptr)){ //if it is not available
                current_trans ^= 0x1; // we revert to the previous transfer
                return; // and return
            }
            // copy the real-time state buffer to the SPI transfer buffer
            memcpy(spi_trans[current_trans].out_buf + 2, real_time_state_buffer, N_CVS_TBD * 4 + N_TRIGS_TBD); // 2 bytes for fingerprint, N_CVS_TBD * 4 bytes for CVs, N_TRIGS_TBD bytes for TRIGs
            real_time_state_buffer_consumed = 1; // set the real-time buffer state to consumed
            mutex_exit(&real_time_mutex); // exit the mutex
        }

        ws_sync_counter = 0; // reset the counter
    }
}

void Midi::SetBypassLegacyMidiParser(bool bypass){
    bypassLegacyMidiParser = bypass;
    midiparser.SetShouldQueue(!bypass); // if we bypass the legacy midi parser, we don't queue the data anymore
}

void Midi::SetRealTimeTrig(uint8_t index, uint8_t value){
    if (index + N_CVS_TBD * 4 >= N_CVS_TBD * 4 + N_TRIGS_TBD) return; // check if index is valid
    real_time_state_buffer[N_CVS_TBD * 4 + index] = value; // set the TRIG value at the index
}

void Midi::SetRealTimeCV(uint8_t index, float value){
    if (index >= N_CVS_TBD) return; // check if index is valid
    float *val = reinterpret_cast<float*>(&real_time_state_buffer[index * 4]);
    *val = value; // set the CV value at the index
}
