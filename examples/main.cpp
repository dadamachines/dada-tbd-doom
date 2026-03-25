extern "C" {
    bool core1_separate_stack = true;
}

#include "Midi.h"
#include "Ui.h"

Midi midi; // MIDI handling
Ui tbd_ui(midi); // UI handling, knows midi by reference, probably not the best design choice but works for now

// this if for debug printfs, uses SDA pin on side TBD connector, GPIO20, PIO uart
//SerialPIO transmitter( 20, SerialPIO::NOPIN );

// the setup function runs once when you press reset or power the board
void setup(){
    //transmitter.begin(115200); // Initialize PIO UART for debug output
    //transmitter.println("CTAG TBD Test started");
    midi.Init(); // Initialize MIDI handling
}

void setup1(){
    tbd_ui.Init(); // Initialize UI handling
}

void loop(){
    //transmitter.println("CTAG TBD Loop");
    //delay(1000);
    midi.Update(); // Update MIDI handling
}

void loop1(){
    tbd_ui.Update(); // Update UI handling
}
