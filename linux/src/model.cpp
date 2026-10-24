#include "model.h"

#include <cstdio>
#include <strings.h>

namespace model {

namespace {

struct Function {
    uint8_t fn;
    const char* key;
    const char* name;
};

const Function kFunctions[] = {
    {0x00, "none", "None"},
    {0x01, "play-pause", "Play / pause"},
    {0x03, "voice-assistant", "Voice assistant"},
    {0x05, "previous", "Previous track"},
    {0x06, "next", "Next track"},
    {0x07, "volume", "Volume"},
    {0x08, "noise-control", "Noise control"},
    {0x0A, "switch-track", "Switch track"},
    {0x0B, "volume-up", "Volume up"},
    {0x0C, "volume-down", "Volume down"},
    {0x0D, "switch-device", "Switch device"},
    {0x11, "game-mode", "Game mode"},
};

}  // namespace

const std::vector<EqPreset>& eq_presets() {
    static const std::vector<EqPreset> presets = {
        {0, "balanced", "Balanced"},
        {2, "bass", "Bass"},
        {1, "bold", "Bold"},
    };
    return presets;
}

const EqPreset* eq_by_id(int id) {
    for (const auto& p : eq_presets())
        if (p.id == id) return &p;
    return nullptr;
}

const EqPreset* eq_by_key(const std::string& key) {
    for (const auto& p : eq_presets())
        if (strcasecmp(p.key, key.c_str()) == 0) return &p;
    return nullptr;
}

const std::vector<GestureSlot>& gesture_slots() {
    static const std::vector<GestureSlot> slots = {
        {0x01, "single", "Single tap", {0x00, 0x01}},
        {0x02, "double", "Double tap", {0x00, 0x01, 0x05, 0x06, 0x03, 0x11}},
        {0x03, "triple", "Triple tap", {0x00, 0x05, 0x06, 0x03, 0x11}},
        // Firmware 113 refuses None here (status 0x01), so the only choice is Switch device.
        {0x06, "hold", "Touch and hold", {0x0D}},
    };
    return slots;
}

const GestureSlot* slot_by_key(const std::string& key) {
    for (const auto& s : gesture_slots())
        if (strcasecmp(s.key, key.c_str()) == 0) return &s;
    return nullptr;
}

const GestureSlot* slot_by_act(uint8_t act) {
    for (const auto& s : gesture_slots())
        if (s.act == act) return &s;
    return nullptr;
}

std::string function_name(uint8_t fn) {
    for (const auto& f : kFunctions)
        if (f.fn == fn) return f.name;
    char buf[32];
    std::snprintf(buf, sizeof buf, "Unknown (0x%02X)", fn);
    return buf;
}

std::string function_key(uint8_t fn) {
    for (const auto& f : kFunctions)
        if (f.fn == fn) return f.key;
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%02x", fn);
    return buf;
}

std::optional<uint8_t> function_by_key(const std::string& key) {
    for (const auto& f : kFunctions)
        if (strcasecmp(f.key, key.c_str()) == 0) return f.fn;
    return std::nullopt;
}

}  // namespace model
