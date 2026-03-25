#include "SpiAPI.h"
#include <cstring>
#include <Arduino.h>
#include <SD.h>

//extern SerialPIO transmitter;

static uint8_t out_buf[2048], in_buf[2048];
// params
uint8_t * const request_type = &out_buf[2]; // request type
uint8_t * const uint8_param_0 = &out_buf[3]; // first request parameter, e.g. channel, favorite number, ...
uint8_t * const uint8_param_1 = &out_buf[4];; // second request parameter, e.g. preset number, ...
int32_t * const int32_param_2 = (int32_t*)&out_buf[5]; // third request parameter, e.g. value, ...
uint8_t* const string_param_3 = (uint8_t*)&out_buf[9]; // fourth request parameter, e.g. plugin name, parameter name, ...
float* const float_param_0 = (float*)&out_buf[3];

void SpiAPI::Init(){
    out_buf[0] = 0xCA;
    out_buf[1] = 0xFE; // fingerprint
}

void SpiAPI::WaitSpiAPIReadyForCmd(){
    cmd_api_spi.WaitUntilP4IsReady();
}

bool SpiAPI::GetSpiAPIReadyForCmd(){
    return cmd_api_spi.GetP4Ready();
}

bool SpiAPI::transmitData(const std::string &data, const RequestType_t reqType){
    uint32_t len = data.length();
    const char* str = data.c_str();
    // fields are: // 0xCA, 0xFE, request type, length (uint32_t), cstring
    *request_type = reqType;
    uint32_t *lengthField = (uint32_t*)(out_buf + 3);
    uint32_t bytes_to_send = 0;
    uint32_t bytes_sent = 0;
    while (len > 0){
        *lengthField = len;
        *request_type = reqType;
        bytes_to_send = len > 2048 - 7 ? 2048 - 7 : len; // 7 bytes for header
        const char* ptr_cstring_section = str + bytes_sent;
        memcpy(out_buf + 7, ptr_cstring_section, bytes_to_send);
        len -= bytes_to_send;
        bytes_sent += bytes_to_send;
        cmd_api_spi.TransferBlockingDelayed(out_buf, in_buf, 2048);

        // fingerprint check
        if (in_buf[0] != 0xCA || in_buf[1] != 0xFE){
            return false;
        }
        // check request type acknowledgment
        const uint8_t requestType = in_buf[2];
        if (requestType != reqType){
            return false;
        }
    }
    return true;
}

bool SpiAPI::receiveData(std::string& response, const RequestType_t request){
    cmd_api_spi.TransferBlockingDelayed(out_buf, in_buf, 2048);

    // fingerprint check
    if (in_buf[0] != 0xCA || in_buf[1] != 0xFE){
        response = "FP0 wrong: " + std::to_string(in_buf[0]) + " " + std::to_string(in_buf[1]);
        return false;
    }

    // check request type acknowledgment
    const uint8_t requestType = in_buf[2];
    if (requestType != request){
        response = "ACK0 wrong: " + std::to_string(requestType);
        return false;
    }

    // read the response
    const uint32_t* resLength = (uint32_t*)&in_buf[3];
    const uint32_t totalResponseLength = *resLength;
    response.reserve(*resLength); // reserve space for the JSON string
    uint32_t bytes_received = *resLength > 2048 - 7 ? 2048 - 7 : *resLength; // 7 bytes for fingerprint and length
    uint32_t bytes_to_be_received = *resLength - bytes_received;
    response.append((char*)&in_buf[7], bytes_received); // skip the first 7 bytes (fingerprint and length)

    while (bytes_to_be_received > 0){
        cmd_api_spi.TransferBlockingDelayed(out_buf, in_buf, 2048);

        // fingerprint check
        if (in_buf[0] != 0xCA || in_buf[1] != 0xFE){
            response = "FP1 wrong: " + std::to_string(in_buf[0]) + " " + std::to_string(in_buf[1]);
            return false;
        }

        // check request type acknowledgment
        const uint8_t requestType = in_buf[2];
        if (requestType != request){
            response = "ACK1 wrong: " + std::to_string(requestType);
            return false;
        }

        // append the received data to the json string
        bytes_received = *resLength > 2048 - 7 ? 2048 - 7 : *resLength; // 7 bytes for fingerprint and length
        response.append((char*)&in_buf[7], bytes_received);
        bytes_to_be_received -= bytes_received;
    }
    if (response.size() != totalResponseLength){
        response = "LEN error: " + std::to_string(totalResponseLength) + ", got " + std::to_string(response.size());
        return false;
    }
    return true;
}

bool SpiAPI::SetActivePlugin(const uint8_t channel, const std::string& pluginID){
    *request_type = RequestType_t::SetActivePlugin; // request type
    *uint8_param_0 = channel; // channel number
    *int32_param_2 = (uint32_t)pluginID.length(); // length of pluginID
    uint8_t* pluginIDField = string_param_3;
    memcpy(pluginIDField, pluginID.c_str(), pluginID.length() + 1); // copy pluginID to buffer, ensure null-termination
    send();
    WaitSpiAPIReadyForCmd();
    delay(10); // wait for TBD to execute the command

    return true;
}

bool SpiAPI::GetActivePlugin(const uint8_t channel, std::string& response){
    // send GetPlugins request
    response.clear();
    *request_type = RequestType_t::GetActivePlugin; // request type
    *uint8_param_0 = channel; // channel number
    send();

    return receiveData(response, RequestType_t::GetActivePlugin);
}

bool SpiAPI::GetPresets(const uint8_t channel, std::string& response){
    response.clear();
    *request_type = RequestType_t::GetPresets; // request type
    *uint8_param_0 = channel; // channel number
    send();

    return receiveData(response, RequestType_t::GetPresets);
}


bool SpiAPI::GetPresetData(const std::string& pluginID, std::string& response){
    response.clear();
    *request_type = RequestType_t::GetPresetData; // request type
    *int32_param_2 = (uint32_t)pluginID.length(); // length of pluginID
    uint8_t* pluginIDField = string_param_3;
    memcpy(pluginIDField, pluginID.c_str(), pluginID.length() + 1); // copy pluginID to buffer, ensure null-termination
    send();

    return receiveData(response, RequestType_t::GetPresetData);
}

bool SpiAPI::LoadPreset(const uint8_t channel, const int8_t presetID){
    *request_type = RequestType_t::LoadPreset; // request type
    *uint8_param_0 = channel; // channel number
    *uint8_param_1 = presetID; // preset ID
    send();
    cmd_api_spi.WaitUntilP4IsReady();

    return true;
}

bool SpiAPI::SavePreset(const uint8_t channel, const std::string & presetName, const int8_t presetID){
    *request_type = RequestType_t::SavePreset; // request type
    *uint8_param_0 = channel; // channel number
    *uint8_param_1 = presetID; // preset ID
    *int32_param_2 = (uint32_t)presetName.length(); // length of presetName
    uint8_t* param_preset_name_field = string_param_3;
    memcpy(param_preset_name_field, presetName.c_str(), presetName.length() + 1);
    send();
    cmd_api_spi.WaitUntilP4IsReady();

    return true;
}


void SpiAPI::send(){
    cmd_api_spi.TransferBlockingDelayed(out_buf, in_buf, 2048);
}

bool SpiAPI::SetPresetData(const std::string& pluginID, const std::string& data){
    *request_type = RequestType_t::SetPresetData;
    uint8_t* param_name_field = string_param_3;
    memcpy(param_name_field, pluginID.c_str(), pluginID.length() + 1);
    send();
    bool res = transmitData(data, RequestType_t::SetPresetData); // send the preset data
    return res;
}

bool SpiAPI::GetActivePluginParams(const uint8_t channel, std::string& response){
    // send GetActivePluginParams request
    response.clear();
    *request_type = RequestType_t::GetActivePluginParams; // request type
    *uint8_param_0 = channel; // channel number
    send();

    return receiveData(response, RequestType_t::GetActivePluginParams);
}

bool SpiAPI::SetActivePluginParam(const uint8_t channel, const std::string& paramName, const int32_t value){
    *request_type = RequestType_t::SetPluginParam; // request type
    *uint8_param_0 = channel; // channel number
    *int32_param_2 = value; // value to set
    uint8_t* param_name_field = string_param_3;
    memcpy(param_name_field, paramName.c_str(), paramName.length() + 1);
    send();
    cmd_api_spi.WaitUntilP4IsReady();
    return true;
}

bool SpiAPI::SetActivePluginCV(const uint8_t channel, const std::string& paramName, const int32_t value){
    *request_type = RequestType_t::SetPluginParamCV; // request type
    *uint8_param_0 = channel; // channel number
    *int32_param_2 = value; // value to set
    uint8_t* param_name_field = string_param_3;
    memcpy(param_name_field, paramName.c_str(), paramName.length() + 1);
    send();
    cmd_api_spi.WaitUntilP4IsReady();
    return true;
}

bool SpiAPI::SetActivePluginTrig(const uint8_t channel, const std::string& paramName, const int32_t value){
    *request_type = RequestType_t::SetPluginParamTRIG; // request type
    *uint8_param_0 = channel; // channel number
    *int32_param_2 = value; // value to set
    uint8_t* param_name_field = string_param_3;
    memcpy(param_name_field, paramName.c_str(), paramName.length() + 1);
    send();
    cmd_api_spi.WaitUntilP4IsReady();
    return true;
}

bool SpiAPI::GetAllFavorites(std::string& response){
    // send GetAllFavorites request
    response.clear();
    *request_type = RequestType_t::GetAllFavorites; // request type
    send();

    return receiveData(response, RequestType_t::GetAllFavorites);
}

bool SpiAPI::SaveFavorite(const uint8_t number, const std::string& favoriteData){
    *request_type = RequestType_t::SaveFavorite;
    *uint8_param_0 = number; // channel number
    send();
    bool res = transmitData(favoriteData, RequestType_t::SaveFavorite); // send the favorite data
    cmd_api_spi.WaitUntilP4IsReady();
    return res;
}

bool SpiAPI::LoadFavorite(const int8_t favoriteID){
    // send LoadFavorite request
    *request_type = RequestType_t::LoadFavorite; // request type
    *uint8_param_0 = favoriteID; // favorite ID
    send();
    cmd_api_spi.WaitUntilP4IsReady();
    return true;
}

bool SpiAPI::GetIOCapabilities(std::string& response){
    response.clear();
    *request_type = RequestType_t::GetIOCapabilities;
    send();
    return receiveData(response, RequestType_t::GetIOCapabilities);
}

bool SpiAPI::GetConfiguration(std::string& response){
    // send GetConfiguration request
    response.clear();
    *request_type = RequestType_t::GetConfiguration; // request type
    send();

    return receiveData(response, RequestType_t::GetConfiguration);
}

bool SpiAPI::SetConfiguration(const std::string& configData){
    *request_type = RequestType_t::SetConfiguration; // request type
    send();
    bool res = transmitData(configData, RequestType_t::SetConfiguration);
    cmd_api_spi.WaitUntilP4IsReady();
    return res;
}


bool SpiAPI::GetPlugins(std::string& response){
    // send GetPlugins request
    response.clear();
    *request_type = RequestType_t::GetPlugins; // request type
    send();

    return receiveData(response, RequestType_t::GetPlugins);
}

bool SpiAPI::Reboot(){
    *request_type = RequestType_t::Reboot;
    send();
    delay(10000);
    return true;
}

bool SpiAPI::RebootIntoOTA1(){
    *request_type = RequestType_t::RebootToOTA1;
    send();
    delay(1000);
    return true;
}

bool SpiAPI::RebootIntoOTAX(const uint8_t slot){
    *request_type = RequestType_t::RebootToOTAX;
    *uint8_param_0 = slot;
    send();
    delay(1000);
    return true;
}

bool SpiAPI::GetSampleRomDescriptor(std::string& response){
    // send GetSampleRomDescriptor request
    response.clear();
    *request_type = RequestType_t::GetSampleRomDescriptor; // request type
    send();
    return receiveData(response, RequestType_t::GetSampleRomDescriptor);
}

bool SpiAPI::SetActiveWaveTableBank(const uint8_t bankIndex){
    *request_type = RequestType_t::SetActiveWaveTableBank; // request type
    *uint8_param_0 = bankIndex; // bank index
    send();
    cmd_api_spi.WaitUntilP4IsReady();

    return true;
}

bool SpiAPI::SetActiveSampleRomBank(const uint8_t bankIndex){
    *request_type = RequestType_t::SetActiveSampleRomBank; // request type
    *uint8_param_0 = bankIndex; // bank index
    send();
    cmd_api_spi.WaitUntilP4IsReady();

    return true;
}

bool SpiAPI::GetFirmwareInfo(std::string& response){
    response.clear();
    *request_type = RequestType_t::GetFirmwareInfo;
    send();
    return receiveData(response, RequestType_t::GetFirmwareInfo);
}

bool SpiAPI::SetAbletonLinkTempo(const float tempo){
    *request_type = RequestType_t::SetAbletonLinkTempo; // request type
    *float_param_0 = tempo;
    send();
    cmd_api_spi.WaitUntilP4IsReady();
    return true;
}

bool SpiAPI::SetAbletonLinkStartStop(const bool isPlaying){
    *request_type = RequestType_t::SetAbletonLinkStartStop; // request type
    *uint8_param_0 = isPlaying ? 1 : 0;
    send();
    cmd_api_spi.WaitUntilP4IsReady();
    return true;
}

// CRC32 calculation matching ESP32's esp_rom_crc32_le
static uint32_t crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len) {
    static const uint32_t crc_table[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c
    };

    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= buf[i];
        crc = (crc >> 4) ^ crc_table[crc & 0x0f];
        crc = (crc >> 4) ^ crc_table[crc & 0x0f];
    }
    return ~crc;
}

bool SpiAPI::SendFile(const std::string& localFilePath, const std::string& remoteFilePath){
    Serial.println("\n[DEBUG] SendFile: Starting file transfer");
    Serial.print("[DEBUG] Local file: ");
    Serial.println(localFilePath.c_str());
    Serial.print("[DEBUG] Remote file: ");
    Serial.println(remoteFilePath.c_str());

    // Open local file from SD card
    File file = SD.open(localFilePath.c_str(), FILE_READ);
    if (!file){
        //transmitter.println("[ERROR] Failed to open local file!");
        return false; // File not found
    }

    uint32_t fileSize = file.size();
    //transmitter.print("[DEBUG] File size: ");
    //transmitter.print(fileSize);
    //transmitter.println(" bytes");

    // Step 1: Notify slave of incoming file transfer
    // one init package looks as follows:
    // 0xCA, 0xFE: Watermark Byte 0, 1
    // request type: Byte 2
    // file length (uint32_t): Byte 3-6
    // total number of chunks (uint32_t): Byte 7-10
    // file name (cstring): Byte 11-n
    // n is 2048 - 11 = 2037 bytes max for file name

    //transmitter.println("[DEBUG] Step 1: Sending init frame to slave...");
    *request_type = RequestType_t::SendFile;
    uint32_t* file_length = (uint32_t*) &out_buf[3];
    *file_length = file.size();
    uint32_t* total_chunks = (uint32_t*) &out_buf[7];
    *total_chunks = file.size() / (2048 - 15); // 15 bytes for header in data packets
    if (file.size() % (2048 - 15)) (*total_chunks)++;
    // Save total_chunks to local variable before buffer gets reused
    const uint32_t totalChunks = *total_chunks;
    char* file_name = (char*)&out_buf[11];
    strcpy(file_name, remoteFilePath.c_str());
    send();

    // Get slave acknowledgment, just repeat send
    //transmitter.println("[DEBUG] Checking slave ACK...");
    *request_type = RequestType_t::SendFile;
    send();

    //transmitter.println("[DEBUG] Checking slave ACK...");
    uint8_t * const request_type_ack = &in_buf[2]; // request type
    if (*request_type_ack != RequestType_t::SendFile){
        //transmitter.print("[ERROR] Slave ACK wrong type: ");
        //transmitter.println(*request_type_ack);
        file.close();
        return false; // Slave did not acknowledge SendFile request
    }
    uint32_t file_size_ack = *(uint32_t*)&in_buf[3];
    //transmitter.print("[DEBUG] Slave ACK file size: ");
    //transmitter.println(file_size_ack);
    if (file_size_ack != fileSize){
        //transmitter.println("[ERROR] Slave ACK file size mismatch!");
        file.close();
        return false; // Slave did not acknowledge correct file size
    }
    //transmitter.println("[DEBUG] Step 1 complete - slave acknowledged");

    // Step 2: Send file data in chunks
    // one sender data package looks as follows:
    // 0xCA, 0xFE: Watermark Byte 0, 1
    // request type: Byte 2
    // chunk number (uint32_t): Byte 3-6
    // chunk data size (uint32_t): Byte 7-10
    // chunk data crc32le (uint32_t): Byte 11-14
    // chunk data: Byte 15-n
    // n is 2048 - 15 = 2033 bytes max for chunk data
    // slave responds in subsequent frame with following acknowledgement
    // 0xCA, 0xFE: Watermark Byte 0, 1
    // request type: Byte 2
    // chunk number (uint32_t): Byte 3-6
    // chunk status (uint8_t): Byte 7 (0 = OK, 1 = CRC error, 2 = other error)
    // on error the sender must restart sending from previous chunk
    // this should only occur on CRC errors, other errors are fatal
    // the slave response is delayed by one transmission
    // except for the first chunk, where general errors are reported immediately
    // retrying should occur only on CRC errors maximum 3 times per chunk
    // we are master

    //transmitter.println("[DEBUG] Step 2: Sending file data...");
    uint32_t* chunk_number_field_sender = (uint32_t*)&out_buf[3];
    uint32_t* chunk_size_field_sender = (uint32_t*)&out_buf[7];
    uint32_t* chunk_crc32_field_sender = (uint32_t*)&out_buf[11];
    uint8_t* chunk_data_field_sender = &out_buf[15];
    uint32_t chunkNumber = 0;
    const uint16_t* watermark_field_receiver = (uint16_t*)&in_buf[0];
    const uint8_t* request_type_field_receiver = &in_buf[2];
    const uint32_t* chunk_number_field_receiver = (uint32_t*)&in_buf[3];
    const uint8_t* chunk_status_field_receiver = &in_buf[7];

    while (file.available() && chunkNumber < totalChunks){
        // Read chunk data from file
        size_t bytesRead = file.read(chunk_data_field_sender, 2048 - 15);
        // Prepare data packet
        *request_type = RequestType_t::SendFile;
        *chunk_number_field_sender = chunkNumber;
        *chunk_size_field_sender = bytesRead;
        *chunk_crc32_field_sender = crc32_le(0, chunk_data_field_sender, bytesRead);
        // Transmit data packet
        send();
        // first chunk can already have an error from slave e.g. if file exists but is corrupted
        // so we need to check for that right away
        if (chunkNumber == 0){
            //transmitter.println("[DEBUG] Checking slave ACK for first chunk...");
            if (*watermark_field_receiver != 0xFECA){
                //transmitter.println("[ERROR] Slave ACK wrong watermark at first chunk");
                file.close();
                return false; // Slave did not acknowledge SendFile request
            }
            if (*request_type_field_receiver != RequestType_t::SendFile){
                //transmitter.println("[ERROR] Slave ACK wrong type at first chunk");
                file.close();
                return false; // Slave did not acknowledge SendFile request
            }
            uint32_t ackChunkNumber = *chunk_number_field_receiver;
            if (ackChunkNumber != 0){
                //transmitter.println("[ERROR] Slave ACK chunk number mismatch at first chunk");
                file.close();
                return false; // Slave did not acknowledge correct chunk number
            }
            uint8_t chunkStatus = *chunk_status_field_receiver;
            if (chunkStatus != 0){
                //transmitter.println("[ERROR] Slave reported fatal error at first chunk");
                file.close();
                return false; // Fatal error reported by slave
            }
        }
        // check slave acknowledgment from previous chunk (if not first chunk)
        if (chunkNumber > 0){
            if (*watermark_field_receiver != 0xFECA){
                //transmitter.print("[ERROR] Slave ACK wrong watermark at chunk ");
                //transmitter.println(chunkNumber - 1);
                file.close();
                return false; // Slave did not acknowledge SendFile request
            }
            if (*request_type_field_receiver != RequestType_t::SendFile){
                //transmitter.print("[ERROR] Slave ACK wrong type at chunk ");
                //transmitter.println(chunkNumber - 1);
                file.close();
                return false; // Slave did not acknowledge SendFile request
            }
            uint32_t ackChunkNumber = *chunk_number_field_receiver;
            if (ackChunkNumber != chunkNumber - 1){
                //transmitter.print("[ERROR] Slave ACK chunk number mismatch at chunk ");
                //transmitter.println(chunkNumber - 1);
                file.close();
                return false; // Slave did not acknowledge correct chunk number
            }
            uint8_t chunkStatus = *chunk_status_field_receiver;
            if (chunkStatus == 1){
                //transmitter.print("[WARNING] Slave reported CRC error at chunk ");
                //transmitter.println(chunkNumber - 1);
                // retry sending previous chunk
                file.seek((chunkNumber - 1) * (2048 - 15)); // seek back to previous chunk
                chunkNumber--; // Decrement to stay on the same chunk number
                continue;
            } else if (chunkStatus == 2){
                //transmitter.print("[ERROR] Slave reported fatal error at chunk ");
                //transmitter.println(chunkNumber - 1);
                file.close();
                return false; // Fatal error reported by slave
            }
        }
        chunkNumber++;
        if (chunkNumber % 10 == 0){
            //transmitter.print("[DEBUG] Sent chunk ");;
            //transmitter.print(chunkNumber);
            //transmitter.print(" / ");
            //transmitter.println(totalChunks);
        }
    }

    // Check final chunk acknowledgment
    send();
    if (*watermark_field_receiver != 0xFECA ||
        *request_type_field_receiver != RequestType_t::SendFile ||
        *chunk_number_field_receiver != chunkNumber - 1 ||
        *chunk_status_field_receiver != 0) {
        //transmitter.println("[ERROR] Final chunk not acknowledged properly");
        file.close();
        return false;
    }

    file.close();
    //transmitter.println("[SUCCESS] File transfer complete!");
    return true;
}