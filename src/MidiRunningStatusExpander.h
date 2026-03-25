#pragma once

class MidiRunningStatusExpander {
public:
    enum class FeedResult {
        MessageComplete,
        BytesMissing,
        SystemRealtimeMessage,
        InvalidMessage
    };

    MidiRunningStatusExpander() : running_status(0), needed_bytes(0), message_len(0) {}

    FeedResult Feed(uint8_t byte) {
        if (byte >= 0xF8 && byte <= 0xFF) return FeedResult::SystemRealtimeMessage; // System realtime messages
        if (byte & 0x80) { // Status byte
            running_status = byte;
            message[0] = byte;
            message_len = 1;
            needed_bytes = DataBytesNeeded(byte);
            if (needed_bytes == 0) {
                message_len = 0;
                return FeedResult::InvalidMessage;
            }
            return FeedResult::BytesMissing;
        } else if (running_status) { // Data byte with running status
            if (message_len == 0) {
                // Start new message with running status
                message[0] = running_status;
                message_len = 1;
                needed_bytes = DataBytesNeeded(running_status);
            }
            if (message_len < 3) {
                message[message_len++] = byte;
            }
            if (--needed_bytes == 0) {
                // Ready for next message, but keep running status
                return FeedResult::MessageComplete;
            } else if (needed_bytes > 0) {
                return FeedResult::BytesMissing;
            } else {
                message_len = 0;
                needed_bytes = 0;
                return FeedResult::InvalidMessage;
            }
        } else {
            // Data byte without running status: invalid
            message_len = 0;
            needed_bytes = 0;
            return FeedResult::InvalidMessage;
        }
    }

    const uint8_t* GetMessage(int &len) {
        len = message_len;
        message_len = 0;
        needed_bytes = 0;
        return message;
    }

    void Reset() {
        message_len = 0;
        needed_bytes = 0;
    }

private:
    uint8_t running_status;
    int needed_bytes;
    uint8_t message[3];
    int message_len;

    int DataBytesNeeded(uint8_t status) {
        switch (status & 0xF0) {
            case 0xC0: // Program Change
            case 0xD0: // Channel Pressure
                return 1;
            case 0x80: // Note Off
            case 0x90: // Note On
            case 0xA0: // Polyphonic Key Pressure
            case 0xB0: // Control Change
            case 0xE0: // Pitch Bend
                return 2;
            default:
                return 0; // System messages not handled
        }
    }
};