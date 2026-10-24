#pragma once

#include <winsock2.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bt {

struct Device {
    std::string name;
    std::string address;  // AA:BB:CC:DD:EE:FF
    uint64_t addr = 0;
    bool connected = false;
};

// Paired devices Windows remembers. *radio is false when there is no Bluetooth adapter (or it is off).
std::vector<Device> paired_devices(bool* radio);

// Opens a blocking RFCOMM socket to a service UUID; Windows looks the channel up over SDP.
// Returns INVALID_SOCKET and sets err on failure ("control service not offered" when SDP has no such service).
SOCKET rfcomm_connect(uint64_t addr, const GUID& service, std::string* err);

// Connects or disconnects the earbuds' audio through the Windows Bluetooth audio driver, the same way
// Settings → Bluetooth does. Blocking: call it off the UI thread.
bool set_audio_connected(const std::string& device_name, bool connect, std::string* err);

}  // namespace bt
