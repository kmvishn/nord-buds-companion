#include "protocol.h"

#include <algorithm>
#include <sstream>

namespace proto {

std::vector<uint8_t> encode(uint16_t cmd, uint8_t seq, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> body = {0x00, 0x00, uint8_t(cmd), uint8_t(cmd >> 8), seq,
                                 uint8_t(payload.size()), uint8_t(payload.size() >> 8)};
    body.insert(body.end(), payload.begin(), payload.end());

    std::vector<uint8_t> frame = {0xAA};
    size_t len = body.size();
    if (len < 0x80) {
        frame.push_back(uint8_t(len));
    } else {
        frame.push_back(uint8_t((len & 0x7F) | 0x80));
        frame.push_back(uint8_t(len >> 7));
    }
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}

void Deframer::feed(const uint8_t* data, size_t len) {
    buf_.insert(buf_.end(), data, data + len);
}

std::optional<Message> Deframer::next() {
    while (true) {
        auto start = std::find(buf_.begin(), buf_.end(), uint8_t(0xAA));
        buf_.erase(buf_.begin(), start);
        if (buf_.size() < 2) return std::nullopt;

        size_t len = buf_[1] & 0x7F;
        size_t header = 2;
        if (buf_[1] & 0x80) {
            if (buf_.size() < 3) return std::nullopt;
            if (buf_[2] & 0x80) {
                buf_.erase(buf_.begin());
                continue;
            }
            len |= size_t(buf_[2]) << 7;
            header = 3;
        }
        if (len < 2 || len > 4096) {
            buf_.erase(buf_.begin());
            continue;
        }
        if (buf_.size() < header + len) return std::nullopt;

        std::vector<uint8_t> body(buf_.begin() + long(header), buf_.begin() + long(header + len));
        buf_.erase(buf_.begin(), buf_.begin() + long(header + len));

        std::vector<uint8_t> msg;
        uint8_t slice = body[0];
        if (slice == 0) {
            msg.assign(body.begin() + 2, body.end());
        } else {
            if (body.size() < 3) continue;
            if (slice == 1) slices_.clear();
            slices_.insert(slices_.end(), body.begin() + 3, body.end());
            if (slice != 3) continue;
            msg.swap(slices_);
            slices_.clear();
        }
        if (msg.size() < 5) continue;

        Message m;
        m.cmd = uint16_t(msg[0] | (msg[1] << 8));
        m.seq = msg[2];
        size_t declared = size_t(msg[3] | (msg[4] << 8));
        size_t available = msg.size() - 5;
        m.payload.assign(msg.begin() + 5, msg.begin() + 5 + long(std::min(declared, available)));
        return m;
    }
}

void parse_battery(const uint8_t* p, size_t n, Battery& out) {
    if (n < 1) return;
    size_t count = p[0];
    for (size_t i = 0; i < count; i++) {
        size_t at = 1 + i * 2;
        if (at + 1 >= n) break;
        uint8_t part = p[at];
        uint8_t raw = p[at + 1];
        BatteryPart value;
        if ((raw & 0x7F) != 0) {
            value.level = raw & 0x7F;
            value.charging = (raw & 0x80) != 0;
        }
        if (part == 1) out.left = value;
        else if (part == 2) out.right = value;
        else if (part == 3) out.box = value;
    }
}

std::optional<Firmware> parse_firmware(const std::vector<uint8_t>& payload) {
    if (payload.size() < 3 || payload[0] != 0) return std::nullopt;
    std::string text(payload.begin() + 2, payload.end());
    while (!text.empty() && (text.back() == '\0' || text.back() == ' ')) text.pop_back();

    std::vector<std::string> parts;
    std::stringstream ss(text);
    for (std::string item; std::getline(ss, item, ',');) parts.push_back(item);

    Firmware fw;
    for (size_t i = 0; i + 2 < parts.size(); i += 3) {
        if (parts[i + 1] != "2") continue;
        if (parts[i] == "1") fw.left = parts[i + 2];
        else if (parts[i] == "2") fw.right = parts[i + 2];
        else if (parts[i] == "3") fw.box = parts[i + 2];
    }
    for (const std::string* v : {&fw.left, &fw.right, &fw.box}) {
        if (v->empty()) continue;
        if (!fw.summary.empty()) fw.summary += ".";
        fw.summary += *v;
    }
    if (fw.summary.empty()) fw.summary = text;
    return fw;
}

std::vector<GestureEntry> parse_gestures(const std::vector<uint8_t>& payload) {
    std::vector<GestureEntry> out;
    if (payload.size() < 2 || payload[0] != 0) return out;
    size_t count = payload[1];
    for (size_t i = 0; i < count; i++) {
        size_t at = 2 + i * 4;
        if (at + 4 > payload.size()) break;
        out.push_back({payload[at], payload[at + 1], payload[at + 2], payload[at + 3]});
    }
    return out;
}

std::vector<uint8_t> encode_gestures(const std::vector<GestureEntry>& entries) {
    std::vector<uint8_t> out = {uint8_t(entries.size())};
    for (const auto& e : entries) {
        out.push_back(e.dev);
        out.push_back(e.button);
        out.push_back(e.act);
        out.push_back(e.fn);
    }
    return out;
}

int bitmap_bit(uint16_t cmd) {
    switch (cmd) {
    case kFirmware: return 0;
    case kGestures:
    case kGesturesSet: return 3;
    case kFind: return 5;
    case kFeatureSet: return 7;
    case kEqSet:
    case kEq: return 10;
    case kSubscribe: return 22;
    default: return -1;
    }
}

bool bitmap_allows(const std::vector<uint8_t>& bitmap, uint16_t cmd) {
    int bit = bitmap_bit(cmd);
    if (bit < 0 || bitmap.empty()) return true;
    size_t byte = size_t(bit) / 8;
    if (byte >= bitmap.size()) return false;
    return (bitmap[byte] >> (bit % 8)) & 1;
}

}  // namespace proto
