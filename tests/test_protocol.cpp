#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "../windows/src/model.h"
#include "../windows/src/protocol.h"

#define TEST_ASSERT(cond)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << std::endl;             \
            std::exit(1);                                                                                              \
        }                                                                                                              \
    } while (0)

void test_encode_and_deframe_single() {
    std::vector<uint8_t> payload = {0x11, 0x22, 0x33, 0x44};
    uint16_t cmd = proto::kBattery;
    uint8_t seq = 42;

    auto frame = proto::encode(cmd, seq, payload);
    TEST_ASSERT(!frame.empty());
    TEST_ASSERT(frame[0] == 0xAA);

    proto::Deframer deframer;
    deframer.feed(frame.data(), frame.size());
    auto msg = deframer.next();
    TEST_ASSERT(msg.has_value());
    TEST_ASSERT(msg->cmd == cmd);
    TEST_ASSERT(msg->seq == seq);
    TEST_ASSERT(msg->payload == payload);
    TEST_ASSERT(!deframer.next().has_value());
}

void test_deframe_fragmented() {
    std::vector<uint8_t> payload(200, 0xAB);
    uint16_t cmd = proto::kFirmware;
    uint8_t seq = 1;

    auto frame = proto::encode(cmd, seq, payload);
    proto::Deframer deframer;

    // Feed byte-by-byte to simulate TCP/stream fragmentation
    for (size_t i = 0; i < frame.size() - 1; ++i) {
        deframer.feed(&frame[i], 1);
        TEST_ASSERT(!deframer.next().has_value());
    }

    deframer.feed(&frame.back(), 1);
    auto msg = deframer.next();
    TEST_ASSERT(msg.has_value());
    TEST_ASSERT(msg->cmd == cmd);
    TEST_ASSERT(msg->seq == seq);
    TEST_ASSERT(msg->payload == payload);
}

void test_deframe_multiple_frames() {
    auto f1 = proto::encode(proto::kBattery, 1, {0x01});
    auto f2 = proto::encode(proto::kEq, 2, {0x02, 0x03});

    std::vector<uint8_t> combined;
    combined.insert(combined.end(), f1.begin(), f1.end());
    combined.insert(combined.end(), f2.begin(), f2.end());

    proto::Deframer deframer;
    deframer.feed(combined.data(), combined.size());

    auto m1 = deframer.next();
    TEST_ASSERT(m1.has_value());
    TEST_ASSERT(m1->cmd == proto::kBattery && m1->seq == 1 && m1->payload.size() == 1);

    auto m2 = deframer.next();
    TEST_ASSERT(m2.has_value());
    TEST_ASSERT(m2->cmd == proto::kEq && m2->seq == 2 && m2->payload.size() == 2);

    TEST_ASSERT(!deframer.next().has_value());
}

void test_parse_battery() {
    proto::Battery bat;
    // Format: <count> (<part> <level|0x80 charging>)...
    // part: 1 = left, 2 = right, 3 = box
    // Left: 90% discharging, Right: 85% charging (0x80 | 85 = 0xD5), Case: 60% discharging
    uint8_t raw[] = {3, 1, 90, 2, 0x80 | 85, 3, 60};

    proto::parse_battery(raw, sizeof(raw), bat);

    TEST_ASSERT(bat.left.level == 90);
    TEST_ASSERT(!bat.left.charging);

    TEST_ASSERT(bat.right.level == 85);
    TEST_ASSERT(bat.right.charging);

    TEST_ASSERT(bat.box.level == 60);
    TEST_ASSERT(!bat.box.charging);
}

void test_parse_firmware() {
    // Expected vendor payload: status 0x00, 0x00, then "1,2,113,2,2,113,3,2,101"
    std::string fw_str = "1,2,113,2,2,113,3,2,101";
    std::vector<uint8_t> payload = {0x00, 0x00};
    payload.insert(payload.end(), fw_str.begin(), fw_str.end());

    auto res = proto::parse_firmware(payload);
    TEST_ASSERT(res.has_value());
    TEST_ASSERT(res->left == "113");
    TEST_ASSERT(res->right == "113");
    TEST_ASSERT(res->box == "101");
    TEST_ASSERT(res->summary == "113.113.101");
}

void test_gestures_roundtrip() {
    // dev: 1=left, 2=right; button=1; act: 1=single, 2=double; fn: action
    std::vector<proto::GestureEntry> entries = {
        {1, 1, 1, 0x01},
        {1, 1, 2, 0x03},
        {2, 1, 1, 0x01},
        {2, 1, 2, 0x04},
    };

    auto encoded = proto::encode_gestures(entries);
    TEST_ASSERT(!encoded.empty());

    // parse_gestures expects status byte 00 at head
    std::vector<uint8_t> reply_payload = {0x00};
    reply_payload.insert(reply_payload.end(), encoded.begin(), encoded.end());

    auto parsed = proto::parse_gestures(reply_payload);
    TEST_ASSERT(parsed.size() == entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        TEST_ASSERT(parsed[i].dev == entries[i].dev);
        TEST_ASSERT(parsed[i].act == entries[i].act);
        TEST_ASSERT(parsed[i].fn == entries[i].fn);
    }
}

void test_bitmap_allows() {
    // 0x0100 bitmap: 1 byte with bits 0..7
    // Let's create a bitmap where bit 0 is set
    std::vector<uint8_t> bmp = {0x01};

    // Bit for kFirmware (0x0105): let's check
    int bit_fw = proto::bitmap_bit(proto::kFirmware);
    TEST_ASSERT(bit_fw >= 0);

    // Negative bit commands are always allowed (e.g. kBattery 0x0106)
    int bit_bat = proto::bitmap_bit(proto::kBattery);
    TEST_ASSERT(bit_bat < 0);
    TEST_ASSERT(proto::bitmap_allows(bmp, proto::kBattery));
}

void test_model() {
    // EQ Presets
    const auto* eq_balanced = model::eq_by_key("balanced");
    TEST_ASSERT(eq_balanced != nullptr && eq_balanced->id == 0);

    const auto* eq_bass = model::eq_by_key("BASS"); // Case insensitive
    TEST_ASSERT(eq_bass != nullptr && eq_bass->id == 2);

    const auto* eq_bold = model::eq_by_id(1);
    TEST_ASSERT(eq_bold != nullptr && std::string(eq_bold->key) == "bold");

    TEST_ASSERT(model::eq_by_key("invalid_eq") == nullptr);

    // Gesture Slots
    const auto* slot_single = model::slot_by_key("single");
    TEST_ASSERT(slot_single != nullptr && slot_single->act == 1);

    const auto* slot_double = model::slot_by_act(2);
    TEST_ASSERT(slot_double != nullptr && std::string(slot_double->key) == "double");

    // Functions
    auto fn_play = model::function_by_key("play-pause");
    TEST_ASSERT(fn_play.has_value());
    TEST_ASSERT(model::function_key(*fn_play) == "play-pause");
    TEST_ASSERT(!model::function_name(*fn_play).empty());
}

int main() {
    std::cout << "Running Nord Buds Protocol and Model Test Suite..." << std::endl;

    test_encode_and_deframe_single();
    std::cout << "  [PASS] test_encode_and_deframe_single" << std::endl;

    test_deframe_fragmented();
    std::cout << "  [PASS] test_deframe_fragmented" << std::endl;

    test_deframe_multiple_frames();
    std::cout << "  [PASS] test_deframe_multiple_frames" << std::endl;

    test_parse_battery();
    std::cout << "  [PASS] test_parse_battery" << std::endl;

    test_parse_firmware();
    std::cout << "  [PASS] test_parse_firmware" << std::endl;

    test_gestures_roundtrip();
    std::cout << "  [PASS] test_gestures_roundtrip" << std::endl;

    test_bitmap_allows();
    std::cout << "  [PASS] test_bitmap_allows" << std::endl;

    test_model();
    std::cout << "  [PASS] test_model" << std::endl;

    std::cout << "All protocol and model tests passed successfully!" << std::endl;
    return 0;
}
