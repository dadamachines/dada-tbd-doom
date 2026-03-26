#include "Ui.h"
#include "SpiAPI.h"
#include "DadaLogo.h"
#include <SD.h>
#include <map>
#include "Fonts/TomThumb.h"

SpiAPI spi_api;

#define STM32RESET_PIN 40
#define SDRESET_PIN 17

#define SDIO_CLK_GPIO 2
#define SDIO_CMD_GPIO 3
#define SDIO_DAT0_GPIO 4

uint8_t psramarray[8*1024*1024] PSRAM;

// compute a 0..1 counter that runs `quantum` times faster than `phase`
static float phased_subcounter(float phase, float quantum){
    // multiply phase by quantum and wrap into [0,1)
    float v = std::fmod(phase * quantum, 1.0f);
    if (v < 0.0f) v += 1.0f; // handle negative phases
    return v;
}

void Ui::Init(){
    InitHardware();
    InitDisplay();
    InitLeds();
    InitSDCard();

    // RunSpiAPITests();
    // RunPSRAMTests();
    // RunSDCardTests();

    // uncomment for an example how to load and map DrumRack for control
    // LoadDrumRackAndMapNoteOnsExample();
    // RealTimeCVTrigAPIExample();
    // GetAndDisplaySampleRomDescriptor_SetToBank1();
    // GetP4FirmwareInfo();
}

void Ui::InitHardware(){
    // reset stm
    pinMode(STM32RESET_PIN, OUTPUT);
    digitalWrite(STM32RESET_PIN, false);
    // reset SD card
    pinMode(SDRESET_PIN, OUTPUT);
    digitalWrite(SDRESET_PIN, true);

    delay(100);
    digitalWrite(STM32RESET_PIN, true);
    digitalWrite(SDRESET_PIN, false);

    // SPI API init
    spi_api.Init();

    // UI STM32 communication
    Wire1.setSDA(I2C_SDA);
    Wire1.setSCL(I2C_SCL);
    Wire1.setClock(400000);
    //Wire1.onFinishedAsync(i2c_async_done);
    Wire1.begin();
}

void Ui::InitDisplay(){
    // display init
    display.begin(0, true);
    display.setRotation(0);
    display.setTextSize(1);
    display.setFont(&TomThumb);
    display.clearDisplay();
    display.display();
}

void Ui::InitLeds(){
    // NeoPixel init
    strip.begin(); // INITIALIZE NeoPixel strip object (REQUIRED)
    strip.show(); // Turn OFF all pixels ASAP
    strip.setBrightness(10);
}

void Ui::InitSDCard(){
    // sd-card with SDIO
    // https://arduino-pico.readthedocs.io/en/latest/fs.html#enabling-sdio-operation-for-sd
    sdInitialized = SD.begin(SDIO_CLK_GPIO, SDIO_CMD_GPIO, SDIO_DAT0_GPIO);
}

void Ui::displayString(const std::string& s){
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1309_WHITE);
    display.setCursor(0, 0);
    display.printf("%s\n", s.c_str());
    display.display();
}

void Ui::displayStringWait1s(const std::string& s){
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1309_WHITE);
    display.setCursor(0, 0);
    display.printf("%s\n", s.c_str());
    display.display();
    delay(1000);
}

void Ui::Poll(){
    static unsigned long lastTime = 0;
    unsigned long currentTime = millis();
    if (currentTime - lastTime < 10) return; // update every 10ms
    lastTime = currentTime;
    p4Ready = midi.GetP4AliveStatus();
    if (!p4Ready){
        // assert reset for stm32
        digitalWrite(STM32RESET_PIN, false);
        resetRequested = true;
        delay(100);
        return;
    }
    // de-assert stm reset
    if (resetRequested){
        resetRequested = false;
        digitalWrite(STM32RESET_PIN, true);
        delay(1000);
    }
    UpdateUIInputsBlocking();
}


void Ui::Update(){
    static unsigned long lastTime = 0;
    unsigned long currentTime = millis();
    if (currentTime - lastTime < 10) return; // update every 10ms
    lastTime = currentTime;
    p4Ready = midi.GetP4AliveStatus();
    if (!p4Ready){
        //displayString("Waiting for P4...");
        display.clearDisplay();
        display.drawBitmap(0, 0, dada_bitmapx, 128, 64, SSD1309_WHITE);
        display.display();
        // assert reset for stm32
        digitalWrite(STM32RESET_PIN, false);
        resetRequested = true;
        delay(100);
        return;
    }
    // de-assert stm reset
    if (resetRequested){
        resetRequested = false;
        doomAudioInitialized = false; // re-init after P4 reboot
        digitalWrite(STM32RESET_PIN, true);
        delay(1000);
    }

    // One-shot: activate PicoAudioBridge plugin on P4
    if (!doomAudioInitialized) {
        doomAudioInitialized = true;
        spi_api.SetActivePlugin(0, "PicoAudioBridge");
        midi.SetDoomAudioMode(true);
    }

    RunUITests();
}

void Ui::RunSpiAPITests(){
    std::string response;

    displayStringWait1s("GetPlugins...");
    spi_api.GetPlugins(response);
    displayStringWait1s(response);

    displayStringWait1s("SetActivePlugin...");
    spi_api.SetActivePlugin(0, "SineSrc");
    spi_api.SetActivePlugin(1, "SineSrc");
    std::string active_plugins;
    displayStringWait1s("GetActivePlugins...");
    spi_api.GetActivePlugin(0, response);
    active_plugins = "CH0: " + response;
    spi_api.GetActivePlugin(1, response);
    active_plugins += " CH1: " + response;
    displayStringWait1s(active_plugins);

    displayStringWait1s("SetActivePluginParam...");
    spi_api.SetActivePluginParam(0, "enableEG", 0);
    spi_api.SetActivePluginParam(1, "enableEG", 0);
    for (int i = 440; i < 1000; i += 20){
        spi_api.SetActivePluginParam(0, "frequency", i);
        spi_api.SetActivePluginParam(1, "frequency", i * 2);
    }
    displayStringWait1s("Save Preset...");
    spi_api.SavePreset(0, "TestPreset0", 1);
    spi_api.SavePreset(1, "TestPreset1", 2);
    displayStringWait1s("Load Preset...");
    spi_api.LoadPreset(0, 0);
    spi_api.LoadPreset(1, 0);

    displayStringWait1s("Get CH0 Plugin Param...");
    spi_api.GetActivePluginParams(0, response);
    displayStringWait1s(response);
    displayStringWait1s("Get CH1 Plugin Param...");
    spi_api.GetActivePluginParams(1, response);
    displayStringWait1s(response);

    displayStringWait1s("Get Presets CH0...");
    spi_api.GetPresets(0, response);
    displayStringWait1s(response);
    displayStringWait1s("Get Presets CH1...");
    spi_api.GetPresets(1, response);
    displayStringWait1s(response);

    displayStringWait1s("Get/Set Preset Data...");
    spi_api.GetPresetData("TBDeep", response);
    displayStringWait1s(response);
    spi_api.SetPresetData("TBDeep", response);

    displayStringWait1s("Get/Set Configuration...");
    spi_api.GetConfiguration(response);
    spi_api.SetConfiguration(response);

    displayStringWait1s("Get All Favorites...");
    spi_api.GetAllFavorites(response);
    displayStringWait1s(response);
    displayStringWait1s("Load Favorite 1...");
    spi_api.LoadFavorite(1);
    displayStringWait1s("Save Favorite 0...");
    response =
        "{\"name\":\"Test123\",\"plug_0\":\"SineSrc\",\"pre_0\":0,\"plug_1\":\"SineSrc\",\"pre_1\":0,\"ustring\":\"Test1234\"}";
    spi_api.SaveFavorite(0, response);

    displayStringWait1s("Get IO Caps...");
    spi_api.GetIOCapabilities(response);
    displayStringWait1s(response);

    displayStringWait1s("Reboot P4...");
    spi_api.Reboot();
}

void Ui::RunPSRAMTests(){
    std::string sz = "PSRAM test, wait...\nSize:" + std::string(std::to_string(rp2040.getPSRAMSize())) + " bytes";
    displayString(sz);
    // fill with random data each half
    for (uint32_t i = 0; i < 8*1024*1024; i++){
        uint8_t r = (uint8_t) (i % 0xFF);
        psramarray[i] = r;
    }
    // compare data
    for (uint32_t i = 0; i < 8*1024*1024; i++){
        if (psramarray[i] != (uint8_t) (i % 0xFF)){
            displayStringWait1s("PSRAM test failed!");
            return;
        }
    }
    displayStringWait1s("PSRAM test passed!");
}

void Ui::RunSDCardTests(){
    // test sd-card read / write performance
    std::string s = "SD card test, wait...\n";
    displayString(s);
    // write and read 32MiB of data and show read/write performance in MiB/s
    const uint32_t testSize = 32 * 1024 * 1024;
    File f = SD.open("sdperf.bin", FILE_WRITE);
    uint8_t buf[1024];
    uint32_t start = millis();
    uint32_t end = millis();
    for (uint32_t i = 0; i < testSize; i+= 1024){
        f.write(buf, 1024);
    }
    end = millis();
    float elapsed = (end - start) / 1000.0f;
    f.close();
    s = "SD card write:\n" + std::to_string(float(testSize) / elapsed / 1024.f / 1024.f) + " MiB/s\n";
    displayStringWait1s(s);
    f = SD.open("sdperf.bin", FILE_READ);
    start = millis();
    for (uint32_t i = 0; i < testSize; i+= 1024){
        f.read(buf, 1024);
    }
    end = millis();
    f.close();
    elapsed = (end - start) / 1000.0f;
    s = "SD card read:\n" + std::to_string(float(testSize) / elapsed / 1024.f / 1024.f) + " MiB/s\n";
    displayStringWait1s(s);
}

bool Ui::UpdateUIInputs(){
    // get data from stm
    if (Wire1.finishedAsync()){
        Wire1.readAsync(I2C_SLAVE_ADDR, &ui_data, sizeof(ui_data_t), true);
        return true;
    }
    return false;
}


void Ui::UpdateUIInputsBlocking(){
    // get data from stm
    while (!Wire1.finishedAsync());
    Wire1.readAsync(I2C_SLAVE_ADDR, &ui_data, sizeof(ui_data_t), true);
}

void Ui::LoadDrumRackAndMapNoteOnsExample(){
    while (!midi.GetP4AliveStatus()) {
        displayString("Waiting for P4...");
        delay(1000);
    }

    std::string res;
    // load drum rack plugin
    displayStringWait1s("Load DrumRack...");
    spi_api.SetActivePlugin(0, "DrumRack");

    // show all available plugin parameters, they come as json from the api

    spi_api.GetActivePluginParams(0, res);
    res = "DrumRack params: " + res;
    displayStringWait1s(res);


    // show capabilities of the TBD, those are the strings for all possible mappings
    // ch10 midi, which is the drum channel, has on position 16 of the boolean mapping options A_75_P_C1 which corresponds to the note C1 on ch 10
    // refer to the current midi implementation here: https://docs.google.com/document/d/1nE06D81PKwmRPWvzO2XH71YJTlkTrWKaK04dhERxskg/edit?tab=t.0
    spi_api.GetIOCapabilities(res);
    res = "IO Caps: " + res;
    displayStringWait1s(res);


    // set automation for boolean params of plugin here triggers, map all instruments of DrumRack from note C1 on ch10 onwards
    // this mapps all instruments of the DrumRack to the note on events of the drum channel 10 starting C1 Ableton nomenclature, which is C0 Elektron Octatrack
    // you should be able to trigger these sounds now from your connected midi controller, if you don't hear anything,
    // maybe your DrumRack default patch still has some mappings, which prevent sound trigger e.g. mutes on, check that in the web editor
    spi_api.SetActivePluginTrig(0, "ab_trigger", 16); // analog bass drum -> A_75_P_C1
    spi_api.SetActivePluginTrig(0, "db_trigger", 17); // digital bass drum -> A_76_P_C#1
    spi_api.SetActivePluginTrig(0, "fmb_trigger", 18); // fm bass drum -> A_77_P_D1
    spi_api.SetActivePluginTrig(0, "as_trigger", 19); // analog snare drum -> A_78_P_D#1
    spi_api.SetActivePluginTrig(0, "ds_trigger", 20); // digital snare drum -> B_75_P_E1
    spi_api.SetActivePluginTrig(0, "hh1_trigger", 21); // hihat 1 -> B_76_P_F1
    spi_api.SetActivePluginTrig(0, "hh2_trigger", 22); // hihat 2 -> B_77_P_F#1
    spi_api.SetActivePluginTrig(0, "rs_trigger", 23); // rimshot -> B_78_P_G1
    spi_api.SetActivePluginTrig(0, "cl_trigger", 24); // clap -> C_75_P_G#1
    spi_api.SetActivePluginTrig(0, "s1_gate", 25); // rompler 1 -> C_76_P_A1
    spi_api.SetActivePluginTrig(0, "s2_gate", 26); // rompler 2 -> C_77_P_A#1
    spi_api.SetActivePluginTrig(0, "s3_gate", 27); // rompler 3 -> C_78_P_B1
    spi_api.SetActivePluginTrig(0, "s3_gate", 28); // rompler 4 -> D_75_P_C2
    // map a CV, again refer to the io capilities data and the midi implementation document
    spi_api.SetActivePluginCV(0, "ab_decay", 8); // analog bass drum decay to ch 10 mod wheel -> A_P_MW_1
}

static void mapBoolAndIntParams(const uint8_t channel, JsonArray const &params, uint8_t &cv, uint8_t &trig, const uint8_t maxCV,
    const uint8_t maxTRIG, std::map<std::string, uint8_t> &cvmap, std::map<std::string, uint8_t> &trigmap){
    for (JsonObject const& param : params) {
        const char* type = param["type"];
        const char* id = param["id"];
        const char* name = param["name"];
        if (type && strcmp(type, "bool") == 0) {
            trigmap[id] = trig;
            spi_api.SetActivePluginTrig(channel, id, trig++);
        }
        if (type && strcmp(type, "int") == 0){
            cvmap[id] = cv;
            spi_api.SetActivePluginCV(channel, id, cv++);
        }
        if (type && strcmp(type, "group") == 0) {
            // attention this is a recursive call!
            mapBoolAndIntParams(channel , param["params"].as<JsonArray>(), cv, trig, maxCV, maxTRIG, cvmap, trigmap);
        }
    }
}

static std::string getParamNameById(JsonArray const& params, std::string const &id){
    // iterate all params to find param with id
    for (JsonObject const& param : params) {
        if (param["id"] == id){
            return param["name"].as<std::string>();
        }
        // check if this is a group and search in the group
        if (param["type"] == "group"){
            std::string res = getParamNameById(param["params"].as<JsonArray>(), id);
            if (!res.empty()) return res;
        }
    }
    return ""; // not found
}

void Ui::RealTimeCVTrigAPIExample(){
    while (!midi.GetP4AliveStatus()) {
        displayString("Waiting for P4...");
        delay(1000);
    }
    midi.SetBypassLegacyMidiParser(true); // disable legacy midi parser, we use direct real-time values in this example

    // load drum rack plugin
    displayString("Load DrumRack...");
    spi_api.SetActivePlugin(0, "DrumRack");
    displayString("Getting all params for plugin...");
    // get all available plugin parameters, they come as json from the api
    std::string res;
    spi_api.GetActivePluginParams(0, res);

    // parse json response
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, res);
    if (error){
        displayStringWait1s("Error parsing json: " + std::string(error.c_str()));
        return;
    }

    // we use two maps to map the parameter id to the cv and trig indices in the real-time state buffer
    std::map<std::string, uint8_t> cv_map;
    std::map<std::string, uint8_t> trig_map;
    std::map<std::string, std::string> cv_map_names;

    // parse parameter json string
    JsonArray params = doc["params"].as<JsonArray>();
    // get plugin name and hint, generic for a plugin
    std::string plugin_name = doc["name"].as<std::string>();
    std::string pluging_hint = doc["hint"].as<std::string>();
    // get the name of the drum, this is a specific parameter for the DrumRack plugin
    std::string ab_drum_name = params[0]["name"].as<std::string>();

    // uncomment this to map all available parameters to cv and trig
    /*
    displayString("Mapping params, wait (takes time)...");
    uint8_t cv = 0;
    uint8_t trig = 0;
    mapBoolAndIntParams(0, params, cv, trig, 240, 60, cv_map, trig_map);
    */

    // use this to map only the parameters we want to use in this example
    // CVs and TRIGs are mapped to the real-time state-buffer indices, which are 240 CVs and 60 TRIGs
    // the state-buffer has linear indices for CVs and TRIGs, so we can map them directly
    // i.e. N_CVS_TBD float CVs and N_TRIGS_TBD uint8_t TRIGs
    displayString("Mapping params, wait...");
    spi_api.SetActivePluginTrig(0, "ab_trigger", 0);
    trig_map["ab_trigger"] = 0;
    cv_map_names["ab_trigger"] = getParamNameById(params, "ab_trigger");
    spi_api.SetActivePluginCV(0, "ab_f0", 0);
    cv_map["ab_f0"] = 0;
    cv_map_names["ab_f0"] = getParamNameById(params, "ab_f0");
    spi_api.SetActivePluginCV(0, "ab_decay", 1);
    cv_map["ab_decay"] = 1;
    cv_map_names["ab_decay"] = getParamNameById(params, "ab_decay");

    // store as preset, so we can load it later and would not have to map the parameters again, i.e. just use spi_api.LoadPreset(...)
    displayString("Saving preset, wait...");
    spi_api.SavePreset(0, "Mapped", 1); // save preset with name DrumRackPreset and id 1
    // load preset to check if it works
    displayString("Loading preset, wait...");
    spi_api.LoadPreset(0, 1); // load preset with id 1

    /* TODO still issues with this on P4 side, it crashes after reloading the plugin after the preset data was set
    // this example shows to to import and rexport all presets for a plugin
    // get the json string of the presets (contains all presets for plugin), this could be stored in a file and automatically loaded on startup
    displayString("Getting preset data, wait...");
    spi_api.GetPresetData("DrumRack", res);
    // set the preset data to the plugin, this will set the parameters to the values stored in the preset
    displayString("Setting preset data, wait...");
    spi_api.SetPresetData("DrumRack", res);
    // load the preset again to check if it works
    displayString("Loading preset again, wait...");
    spi_api.LoadPreset(0, 1); // load preset with id 1
    */

    // trigger sounds
    // note if you don't hear anything, maybe your DrumRack default patch still has some mappings, which prevent sound trigger e.g. mutes on, check that in the web editor
    while (1){
        // acquire real-time mutex blocking, this will block until the mutex is available
        midi.AcquireRealTimeMutexBlocking();
        // trigger sounds, CVs and TRIGs have been linearly mapped before to the real-time state buffer
        // note on
        midi.SetRealTimeTrig(trig_map["ab_trigger"], 1); // analog bass drum
        midi.SetRealTimeCV(cv_map["ab_f0"], 0.2f); // analog bass drum tone
        midi.SetRealTimeCV(cv_map["ab_decay"], 0.2f); // digital bass drum
        midi.ReleaseRealTimeMutex();
        displayString( plugin_name + "\n" + pluging_hint + "\n" + ab_drum_name + "\n" + cv_map_names["ab_trigger"] + " on\n" + cv_map_names["ab_f0"] + ": 0.2\n" + cv_map_names["ab_decay"] + ": 0.2");
        delay(500);
        while (!midi.IsRealTimeBufferConsumed()); // wait until the real-time buffer is consumed
        // note off
        midi.AcquireRealTimeMutexBlocking();
        midi.SetRealTimeTrig(trig_map["ab_trigger"], 0); // analog bass drum
        midi.ReleaseRealTimeMutex();
        while (!midi.IsRealTimeBufferConsumed()); // wait until the real-time buffer is consumed
        // note on
        midi.AcquireRealTimeMutexBlocking();
        midi.SetRealTimeTrig(trig_map["ab_trigger"], 1); // analog bass drum
        midi.SetRealTimeCV(cv_map["ab_f0"], 0.4f); // analog bass drum tone
        midi.SetRealTimeCV(cv_map["ab_decay"], 0.5f); // digital bass drum
        midi.ReleaseRealTimeMutex();
        displayString( plugin_name + "\n" + pluging_hint + "\n" + ab_drum_name + "\n" + cv_map_names["ab_trigger"] + " on\n" + cv_map_names["ab_f0"] + ": 0.4\n" + cv_map_names["ab_decay"] + ": 0.5");
        delay(500);
        while (!midi.IsRealTimeBufferConsumed()); // wait until the real-time buffer is consumed
        // note off
        midi.AcquireRealTimeMutexBlocking();
        midi.SetRealTimeTrig(trig_map["ab_trigger"], 0); // analog bass drum
        midi.ReleaseRealTimeMutex();
        while (!midi.IsRealTimeBufferConsumed()); // wait until the real-time buffer is consumed
    }
}

void Ui::BootIntoOTA1(){
    digitalWrite(STM32RESET_PIN, false);
    resetRequested = true;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1309_WHITE);
    display.setCursor(0, 0);
    display.printf("P4 sd-card mode, wait\n");
    display.display();
    spi_api.RebootIntoOTAX(1);
    delay(2000);
    std::string res;
    spi_api.GetFirmwareInfo(res);
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, res);
    if (error){
        displayStringWait1s("Error parsing json: " + std::string(error.c_str()));
    }
    // check if json object key FWV==tusb_msc_1.1 and OTA==ota1
    if (doc["OTA"].as<std::string>() == "ota1"){
        displayStringWait1s("usb-msc active on P4\nde-mount sd to reboot tbd");
    }
    else{
        displayStringWait1s("Boot into OTA1 failed");
    }
    do{
        /*
        static int cnt=0;
        displayStringWait1s("Get Firmware Info..." + std::to_string(cnt++));

        displayStringWait1s(res);
        */
        spi_api.GetFirmwareInfo(res);
        delay(1000);
        deserializeJson(doc, res);
    }while (doc["OTA"].as<std::string>() != "ota0");


}

void Ui::GetAndDisplaySampleRomDescriptor_SetToBank1(){
    while (!midi.GetP4AliveStatus()) {
        displayString("Waiting for P4...");
        delay(1000);
    }
    std::string response;
    displayStringWait1s("Get Sample Rom Desc...");
    spi_api.GetSampleRomDescriptor(response);
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);
    displayStringWait1s(response);
    if (error){
        displayStringWait1s("Error parsing json: " + std::string(error.c_str()));
    }
    if (!doc["smp_bank_names"].is<JsonArray>()){
        displayStringWait1s("No sample banks found");
        return;
    }
    JsonArray bank_names = doc["smp_bank_names"].as<JsonArray>();
    if (bank_names.size() < 2){
        displayStringWait1s("Only default bank available");
        return;
    }
    displayStringWait1s("Selecting bank\n" + bank_names[0].as<std::string>() + "\nwait...");
    spi_api.SetActiveSampleRomBank(0);
    spi_api.WaitSpiAPIReadyForCmd();
}

void Ui::GetP4FirmwareInfo(){
    std::string response;
    spi_api.GetFirmwareInfo(response);
    displayStringWait1s(std::string("P4 Firmware Info: ") + response);
    sleep_ms(3000);
}

void Ui::RunUITests(){
    static unsigned long tick = 0;
    unsigned long delta = millis() - tick;
    tick = millis();
    static uint32_t bpm = 0;
    static uint8_t tickLED = 0;
    static bool wasInOTA1 = false;

    ui_data_t ui_data_current = CopyUiData(); // copy current ui data
    // start background DMA ui_data update
    UpdateUIInputsBlocking();

    char buf[64];

    // check for boot into ota1 request
    if (ui_data_current.mcl_btns_long_press & (1 << 8) &&
        ui_data_current.mcl_btns_long_press & (1 << 9) &&
        ui_data_current.mcl_btns_long_press & (1 << 10) &&
        ui_data_current.mcl_btns_long_press & (1 << 11) &&
        !wasInOTA1
        ){
        BootIntoOTA1();
        // required to flush buttons, somehow it get's stuck
        UpdateUIInputsBlocking();
        delay(10);
        UpdateUIInputsBlocking();
        delay(10);
        wasInOTA1 = true;
        return;
    }

    wasInOTA1 = false;
    // print pots
    display.clearDisplay();
    display.setTextColor(SSD1309_WHITE);
    display.setCursor(0, 6);
    snprintf(buf, sizeof(buf), "%04u %04u %04u %04u",
                            ui_data_current.pot_positions[0],
                            ui_data_current.pot_positions[1],
                            ui_data_current.pot_positions[2],
                            ui_data_current.pot_positions[3]);
    display.printf("%s\n", buf);
    for (int i = 0, j = 0; i < 4; i++){
        if (ui_data_current.pot_states[i] & (1 << 0)) buf[j++] = '1';
        else buf[j++] = '0';
        if (ui_data_current.pot_states[i] & (1 << 1)) buf[j++] = '1';
        else buf[j++] = '0';
    }
    buf[8] = 0;
    display.printf("%s\n", buf);

    // print dbuttons
    for (int i = 0; i < 16; i++){
        if (ui_data_current.d_btns & (1 << i)){
            buf[i] = '1';
            strip.setPixelColor(rgb_led_btn_map[i], strip.Color(0, 255, 0));
        }
        else{
            buf[i] = '0';
            if (i % 4 == 0) strip.setPixelColor(rgb_led_btn_map[i], strip.Color(0, 200, 0));
            else strip.setPixelColor(rgb_led_btn_map[i], strip.Color(64, 64, 64));
        }
        if (ui_data_current.d_btns_long_press & (1 << i)){
            buf[i] = 'L';
            strip.setPixelColor(rgb_led_btn_map[i], strip.Color(255, 0, 0));
        }
    }
    buf[16] = 0;
    display.printf("%s\n", buf);

    // print fbuttons
    if (ui_data_current.f_btns & (1 << 1))
        strip.setPixelColor(rgb_led_fbtn_map[1], strip.Color(0, 255, 0));
    else
        strip.setPixelColor(rgb_led_fbtn_map[1], strip.Color(64, 64, 64));
    if (ui_data_current.f_btns_long_press & (1 << 1))
        strip.setPixelColor(rgb_led_fbtn_map[1], strip.Color(255, 0, 0));

    if (ui_data_current.f_btns & (1 << 0))
        strip.setPixelColor(rgb_led_fbtn_map[2], strip.Color(0, 255, 0));
    else
        strip.setPixelColor(rgb_led_fbtn_map[2], strip.Color(64, 64, 64));
    if (ui_data_current.f_btns_long_press & (1 << 0))
        strip.setPixelColor(rgb_led_fbtn_map[2], strip.Color(255, 0, 0));

    ledStatus = midi.GetLedStatus();
    uint8_t b = ledStatus & 0xff;
    uint8_t g = (ledStatus >> 8) & 0xff;
    uint8_t r = (ledStatus >> 16) & 0xff;
    strip.setPixelColor(rgb_led_fbtn_map[0], strip.Color(r, g, b));
    //display.printf("%d\n", ledStatus);

    for (int i = 0; i < 6; i++){
        if (ui_data_current.f_btns & (1 << i)){
            buf[i] = '1';
        }
        else{
            buf[i] = '0';
        }
        if (ui_data_current.f_btns_long_press & (1 << i)){
            buf[i] = 'L';
        }
    }
    buf[6] = 0;
    // f_btns and accelerometer
    display.printf("%s %+04d %+04d %+04d\n", buf, ui_data_current.accelerometer[0]>>8, ui_data_current.accelerometer[1]>>8, ui_data_current.accelerometer[2]>>8);

    // print mcl buttons
    for (int i = 0; i < 12; i++){
        if (ui_data_current.mcl_btns & (1 << i)){
            buf[i] = '1';
        }
        else{
            buf[i] = '0';
        }
        if (ui_data_current.mcl_btns_long_press & (1 << i)){
            buf[i] = 'L';
        }
    }
    buf[12] = 0;
    display.printf("%s\n", buf);

    if (sdInitialized){
        display.printf("FPS %dHz SD OK\n", 1000 / delta);
    }
    else{
        display.printf("FPS %dHz NO SD\n", 1000 / delta);
    }

    link_session_data_t link_data;
    midi.GetLinkData(link_data);
    if (link_data.linkActive){
        display.printf("Ableton %d peers, %3.2fbpm, %s\nphase %1.2f, beat %.1f\n", link_data.numPeers, link_data.tempo, link_data.isPlaying ? "play": "stop", link_data.phase, link_data.beat);
        // start ableton link on left display btn
        if (ui_data_current.f_btns & (1 << 0)){
            spi_api.SetAbletonLinkStartStop(true);
        }
        // stop ableton link on right display btn
        if (ui_data_current.f_btns & (1 << 1)){
            spi_api.SetAbletonLinkStartStop(false);
        }
        // set ableton link bpm with left pot
        if (ui_data_current.pot_states[0] & (1 << 0))
            spi_api.SetAbletonLinkTempo(link_data.tempo + 0.1f);
        if (ui_data_current.pot_states[0] & (1 << 1))
            spi_api.SetAbletonLinkTempo(link_data.tempo - 0.1f);
        // reset tempo to 120 bpm on long press of left pot
        if (ui_data_current.f_btns_long_press & (1 << 2)){
            spi_api.SetAbletonLinkTempo(120.f);
        }
    }else{
        display.printf("Ableton Link not active!\n");
    }

    // in level bar
    uint16_t cy = display.getCursorY();
    display.fillRect(0, cy, r >> 1, 4, SSD1309_WHITE);
    display.fillRect(0, cy + 5, g >> 1, 4, SSD1309_WHITE);
    if (b) display.fillCircle(128 - 4, 3, 2, SSD1309_WHITE);
    else display.drawCircle(128 - 4, 3, 2, SSD1309_WHITE);

    // bpm indicator
    if (link_data.linkActive){
        static bool link_tick {false};
        auto sub_phase {phased_subcounter(link_data.phase, link_data.quantum)};
        if (sub_phase > 0.1f){
            link_tick = false;
        }
        if (sub_phase <= 0.1f && !link_tick){
            tickLED = (tickLED + 1) % 16; // cycle through the LEDs
            link_tick = true;
        }
        if (link_data.phase <= 0.1f){
            strip.setPixelColor(rgb_led_fbtn_map[1], strip.Color(255, 255, 255));
        }
        strip.setPixelColor(rgb_led_btn_map[tickLED], strip.Color(255, 0, 0)); // blink the current tick LED
    }else{
        if (bpm % 20 == 0){
            tickLED = (tickLED + 1) % 16; // cycle through the LEDs
        }
        if (bpm % 80 == 60){
            strip.setPixelColor(rgb_led_fbtn_map[1], strip.Color(255, 255, 255));
        }
        strip.setPixelColor(rgb_led_btn_map[tickLED], strip.Color(255, 0, 0)); // blink the current tick LED
        bpm++;
    }

    display.display();
    strip.show();
}
