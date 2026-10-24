#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// OPPO / OnePlus earbud RFCOMM protocol.
// Frame: AA <len LEB128> <slice> 00 [<fsn>] <cmd u16 LE> <seq> <paylen u16 LE> <payload>
namespace proto {

enum Cmd : uint16_t {
    kBitmap = 0x0100,
    kFirmware = 0x0105,
    kBattery = 0x0106,
    kGestures = 0x0108,
    kFeatures = 0x010D,
    kEq = 0x010F,
    kEventsOffered = 0x0200,
    kEventRegister = 0x0201,
    kPush = 0x0204,
    kSubscribe = 0x0205,
    kFind = 0x0400,
    kGesturesSet = 0x0401,
    kFeatureSet = 0x0403,
    kEqSet = 0x0406,
    kBudTimeRequest = 0x0500,
    kBudEqChanged = 0x0504,
    kBudForegroundApp = 0x050C,
    kBudAppCapabilities = 0x051C,
    kBudSpatialMode = 0x051D,
};

constexpr uint16_t kReplyBit = 0x8000;
constexpr uint8_t kFeatureGameMode = 0x06;

enum PushCode : uint8_t {
    kPushBattery = 0x01,
    kPushWearing = 0x02,
    kPushGameMode = 0x05,
    kPushDevicesInfo = 0xF2,
    kPushDiagnostic = 0xF4,
};

struct Message {
    uint16_t cmd = 0;
    uint8_t seq = 0;
    std::vector<uint8_t> payload;
};

std::vector<uint8_t> encode(uint16_t cmd, uint8_t seq, const std::vector<uint8_t>& payload);

class Deframer {
public:
    void feed(const uint8_t* data, size_t len);
    std::optional<Message> next();

private:
    std::vector<uint8_t> buf_;
    std::vector<uint8_t> slices_;
};

struct BatteryPart {
    int level = -1;  // -1 = not reported
    bool charging = false;
};

struct Battery {
    BatteryPart left, right, box;
};

// p points at <count>, followed by <part> <raw> pairs. Updates only the parts listed.
void parse_battery(const uint8_t* p, size_t n, Battery& out);

struct Firmware {
    std::string left, right, box;
    std::string summary;  // e.g. "113.113.101"
};
std::optional<Firmware> parse_firmware(const std::vector<uint8_t>& payload);

struct GestureEntry {
    uint8_t dev = 0;     // 01 left, 02 right
    uint8_t button = 0;  // 01 main gestures
    uint8_t act = 0;     // 01 single, 02 double, 03 triple, 04 hold, 05 slide, 06 long hold
    uint8_t fn = 0;      // function code
};
std::vector<GestureEntry> parse_gestures(const std::vector<uint8_t>& payload);
std::vector<uint8_t> encode_gestures(const std::vector<GestureEntry>& entries);

// Which capability bit gates a command; -1 when the command is always allowed.
int bitmap_bit(uint16_t cmd);
bool bitmap_allows(const std::vector<uint8_t>& bitmap, uint16_t cmd);

}  // namespace proto
