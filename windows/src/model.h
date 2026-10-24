#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// OnePlus Nord Buds 2r (product 062C14): presets and gesture options as the vendor app offers them.
namespace model {

constexpr uint8_t kLeft = 0x01;
constexpr uint8_t kRight = 0x02;

struct EqPreset {
    uint8_t id;
    const char* key;
    const char* name;
};
const std::vector<EqPreset>& eq_presets();
const EqPreset* eq_by_id(int id);
const EqPreset* eq_by_key(const std::string& key);

struct GestureSlot {
    uint8_t act;
    const char* key;
    const char* name;
    std::vector<uint8_t> options;
};
const std::vector<GestureSlot>& gesture_slots();
const GestureSlot* slot_by_key(const std::string& key);
const GestureSlot* slot_by_act(uint8_t act);

std::string function_name(uint8_t fn);
std::string function_key(uint8_t fn);
std::optional<uint8_t> function_by_key(const std::string& key);

}  // namespace model
