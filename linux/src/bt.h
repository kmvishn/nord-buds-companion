#pragma once

#include <cstdint>
#include <string>

namespace bt {

// Looks up the RFCOMM channel of a 128-bit service UUID over SDP. Returns -1 and sets err on failure.
int sdp_rfcomm_channel(const std::string& address, const uint8_t uuid[16], std::string* err);

// Opens a non-blocking RFCOMM socket. Returns the fd, or -1 and sets err.
int rfcomm_connect(const std::string& address, int channel, std::string* err);

}  // namespace bt
