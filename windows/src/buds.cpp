#include "buds.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <iterator>

#include "bluetooth.h"
#include "util.h"

namespace {

// The vendor control service, in the order the vendor app tries them.
const GUID kVendorServices[] = {
    {0x0000079a, 0xd102, 0x11e1, {0x9b, 0x23, 0x00, 0x02, 0x5b, 0x00, 0xa5, 0xa5}},
    {0x00001107, 0xd102, 0x11e1, {0x9b, 0x23, 0x00, 0x02, 0x5b, 0x00, 0xa5, 0xa5}},
};

const UINT kRetryDelays[] = {2, 5, 10, 30};

enum : UINT { kMsgDevices = WM_APP + 1, kMsgConnected, kMsgData, kMsgClosed, kMsgAudio };
enum : UINT_PTR { kTimerRetry = 1, kTimerInit, kTimerPoll, kTimerFind, kTimerPump, kTimerDisconnect };

constexpr wchar_t kClass[] = L"NordBudsLink";

struct ConnectResult {
    unsigned gen;
    SOCKET sock;
    std::string err;
};

struct Chunk {
    unsigned gen;
    std::vector<uint8_t> bytes;
};

struct AudioResult {
    bool connect;
    bool ok;
    std::string err;
};

std::string hex_byte(uint8_t b) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "0x%02X", b);
    return buf;
}

}  // namespace

Buds::Buds(std::string preferred_address) : preferred_(std::move(preferred_address)) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    hwnd_ = CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

Buds::~Buds() {
    stopping_ = true;
    if (monitor_) {
        SetEvent(wake_);
        WaitForSingleObject(monitor_, 10000);
        CloseHandle(monitor_);
    }
    close_link();
    if (hwnd_) DestroyWindow(hwnd_);
    if (wake_) CloseHandle(wake_);
}

void Buds::notify() {
    for (auto& fn : listeners_) fn();
}

void Buds::fail(const std::string& message) {
    st_.last_error = message;
    st_.error_serial++;
}

void Buds::start() {
    monitor_ = CreateThread(
        nullptr, 0,
        [](void* self) -> DWORD {
            static_cast<Buds*>(self)->monitor_loop();
            return 0;
        },
        this, 0, nullptr);
    if (!monitor_) {
        fail("Could not start the Bluetooth monitor: " + util::error_text(GetLastError()));
        notify();
    }
}

// Windows has no change signal for "device connected" that works for every adapter driver,
// so a worker thread polls the paired-device list and posts it when something changed.
void Buds::monitor_loop() {
    Pick last;
    bool first = true;
    while (!stopping_) {
        bool radio = true;
        auto devices = bt::paired_devices(&radio);
        Pick best;
        best.radio = radio;
        int best_score = -1;
        for (const auto& d : devices) {
            int score = -1;
            if (!preferred_.empty()) {
                if (_stricmp(d.address.c_str(), preferred_.c_str()) == 0) score = 1;
            } else if (d.name.find("Nord Buds") != std::string::npos) {
                score = 1;
            }
            if (score >= 0 && d.connected) score += 2;
            if (score > best_score) {
                best_score = score;
                best = {true, d.name, d.address, d.addr, d.connected, radio};
            }
        }
        if (first || !(best == last)) {
            auto* pick = new Pick(best);
            if (!PostMessageW(hwnd_, kMsgDevices, 0, reinterpret_cast<LPARAM>(pick))) delete pick;
            last = best;
            first = false;
        }
        WaitForSingleObject(wake_, 2000);
    }
}

LRESULT CALLBACK Buds::wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<Buds*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case kMsgDevices: {
        auto* pick = reinterpret_cast<Pick*>(lp);
        self->on_devices(*pick);
        delete pick;
        return 0;
    }
    case kMsgConnected: {
        auto* r = reinterpret_cast<ConnectResult*>(lp);
        self->on_connected(r->gen, r->sock, r->err);
        delete r;
        return 0;
    }
    case kMsgData: {
        auto* c = reinterpret_cast<Chunk*>(lp);
        self->on_data(c->gen, c->bytes);
        delete c;
        return 0;
    }
    case kMsgClosed:
        if (unsigned(wp) == self->gen_ && self->link_)
            self->drop_link(lp == 0 ? "The earbuds closed the control channel" : "Control channel read error");
        return 0;
    case kMsgAudio: {
        auto* r = reinterpret_cast<AudioResult*>(lp);
        if (!r->ok) {
            self->fail("Bluetooth: " + r->err);
            if (!r->connect && self->st_.disconnecting) {
                // Nothing will disconnect, so stop waiting and bring the control channel back.
                self->stop_timer(kTimerDisconnect);
                self->st_.disconnecting = false;
                if (self->st_.bt_connected && !self->link_ && !self->connecting_) self->start_connect();
            }
        }
        delete r;
        SetEvent(self->wake_);
        self->notify();
        return 0;
    }
    case WM_TIMER:
        self->on_timer(UINT_PTR(wp));
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void Buds::start_timer(UINT_PTR id, UINT ms) {
    SetTimer(hwnd_, id, ms, nullptr);
    timers_.insert(id);
}

void Buds::stop_timer(UINT_PTR id) {
    if (timers_.erase(id)) KillTimer(hwnd_, id);
}

void Buds::on_timer(UINT_PTR id) {
    switch (id) {
    case kTimerRetry:
        stop_timer(id);
        if (st_.bt_connected && !st_.disconnecting && !link_ && !connecting_) start_connect();
        notify();
        break;
    case kTimerInit:
        stop_timer(id);
        finish_init();
        break;
    case kTimerPoll:
        if (!ready()) break;
        // Two unanswered polls: the link is dead even if the socket never said so.
        if (GetTickCount64() - last_rx_ > 150 * 1000) {
            stop_timer(id);
            drop_link("The earbuds stopped responding");
            break;
        }
        send(proto::kBattery);
        break;
    case kTimerFind:
        stop_timer(id);
        set_find(false);
        break;
    case kTimerPump:
        pump();
        break;
    case kTimerDisconnect:
        stop_timer(id);
        if (st_.disconnecting) {
            st_.disconnecting = false;
            fail("Windows kept the earbuds connected");
            if (st_.bt_connected && !link_ && !connecting_) start_connect();
            notify();
        }
        break;
    default:
        break;
    }
}

void Buds::on_devices(const Pick& pick) {
    st_.radio = pick.radio;
    if (!pick.found) {
        close_link();
        stop_timer(kTimerRetry);
        addr_ = 0;
        st_.link = LinkState::NoDevice;
        st_.name.clear();
        st_.address.clear();
        st_.bt_connected = false;
        st_.disconnecting = false;
        notify();
        return;
    }
    if (pick.addr != addr_) {
        close_link();
        addr_ = pick.addr;
        st_.bt_connected = false;
    }
    st_.name = pick.name;
    st_.address = pick.address;
    st_.bt_connected = pick.connected;

    if (st_.bt_connected && !st_.disconnecting) {
        if (!link_ && !connecting_ && !timer_on(kTimerRetry)) start_connect();
    } else {
        // Disconnected, or on its way out after our request: an open RFCOMM channel would keep the
        // Bluetooth link alive, so let go of it.
        close_link();
        stop_timer(kTimerRetry);
        retries_ = 0;
        st_.link = LinkState::Disconnected;
        if (!st_.bt_connected) {
            st_.disconnecting = false;
            stop_timer(kTimerDisconnect);
        }
    }
    notify();
}

void Buds::start_connect() {
    connecting_ = true;
    st_.link = LinkState::Connecting;
    unsigned gen = ++gen_;
    HWND hwnd = hwnd_;
    uint64_t addr = addr_;
    bool started = util::run_detached([hwnd, gen, addr] {
        std::string err, last = "control service not found";
        SOCKET sock = INVALID_SOCKET;
        for (const GUID& service : kVendorServices) {
            sock = bt::rfcomm_connect(addr, service, &err);
            if (sock != INVALID_SOCKET) break;
            if (err != "control service not offered") last = err;
        }
        auto* r = new ConnectResult{gen, sock, sock == INVALID_SOCKET ? last : std::string()};
        if (!PostMessageW(hwnd, kMsgConnected, 0, reinterpret_cast<LPARAM>(r))) {
            if (sock != INVALID_SOCKET) closesocket(sock);
            delete r;
        }
    });
    if (!started) on_connected(gen, INVALID_SOCKET, "could not start a thread");
}

void Buds::on_connected(unsigned gen, SOCKET sock, const std::string& err) {
    if (gen != gen_) {
        if (sock != INVALID_SOCKET) closesocket(sock);
        return;
    }
    connecting_ = false;
    if (sock == INVALID_SOCKET) {
        fail("Could not open the control channel: " + err);
        if (st_.bt_connected) schedule_retry();
        notify();
        return;
    }
    if (!st_.bt_connected || st_.disconnecting) {
        closesocket(sock);
        return;
    }
    open_link(sock);
}

void Buds::schedule_retry() {
    if (timer_on(kTimerRetry)) return;
    st_.link = LinkState::Connecting;
    UINT delay = kRetryDelays[std::min<size_t>(size_t(retries_), std::size(kRetryDelays) - 1)];
    retries_++;
    start_timer(kTimerRetry, delay * 1000);
}

void Buds::open_link(SOCKET sock) {
    link_ = std::make_shared<Link>();
    link_->sock = sock;
    retries_ = 0;
    st_.last_error.clear();
    st_.link = LinkState::Connected;
    st_.initialized = false;
    deframer_ = proto::Deframer();
    last_rx_ = GetTickCount64();

    std::shared_ptr<Link> link = link_;
    HWND hwnd = hwnd_;
    unsigned gen = gen_;
    bool started = util::run_detached([link, hwnd, gen] {
        uint8_t buf[2048];
        while (true) {
            // select() with a timeout so a shutdown that does not wake recv() is still noticed.
            fd_set set;
            FD_ZERO(&set);
            FD_SET(link->sock, &set);
            timeval tv = {1, 0};
            int r = select(0, &set, nullptr, nullptr, &tv);
            AcquireSRWLockShared(&link->lock);
            bool closed = link->closed;
            ReleaseSRWLockShared(&link->lock);
            if (closed) break;
            if (r == 0) continue;
            int n = r < 0 ? -1 : recv(link->sock, reinterpret_cast<char*>(buf), sizeof buf, 0);
            if (n > 0) {
                auto* chunk = new Chunk{gen, std::vector<uint8_t>(buf, buf + n)};
                if (PostMessageW(hwnd, kMsgData, gen, reinterpret_cast<LPARAM>(chunk))) continue;
                delete chunk;
                break;
            }
            PostMessageW(hwnd, kMsgClosed, gen, n == 0 ? 0 : 1);
            break;
        }
        AcquireSRWLockExclusive(&link->lock);
        closesocket(link->sock);
        link->sock = INVALID_SOCKET;
        ReleaseSRWLockExclusive(&link->lock);
    });
    if (!started) {
        closesocket(sock);
        link_.reset();
        drop_link("Could not start the control channel reader");
        return;
    }

    send(proto::kBitmap, {}, true);
    send(proto::kEventsOffered, {}, true);
    start_timer(kTimerInit, 6000);
    start_timer(kTimerPoll, 60000);
    notify();
}

void Buds::close_link() {
    gen_++;
    connecting_ = false;
    for (UINT_PTR id : {kTimerPump, kTimerInit, kTimerPoll, kTimerFind}) stop_timer(id);
    if (link_) {
        AcquireSRWLockExclusive(&link_->lock);
        link_->closed = true;
        if (link_->sock != INVALID_SOCKET) shutdown(link_->sock, SD_BOTH);
        ReleaseSRWLockExclusive(&link_->lock);
        link_.reset();
    }
    queue_.clear();
    init_pending_.clear();
    offered_.clear();
    st_.initialized = false;
    st_.battery = {};
    st_.firmware = {};
    st_.eq = -1;
    st_.game_mode = -1;
    st_.finding = false;
    st_.gestures.clear();
    st_.bitmap.clear();
}

void Buds::drop_link(const std::string& reason) {
    close_link();
    if (st_.bt_connected && !st_.disconnecting) {
        fail(reason);
        schedule_retry();
    } else {
        st_.link = LinkState::Disconnected;
    }
    notify();
}

void Buds::send(uint16_t cmd, std::vector<uint8_t> payload, bool init) {
    if (!link_) return;
    queue_.push_back(proto::encode(cmd, seq_, payload));
    seq_ = seq_ >= 0xFE ? 1 : seq_ + 1;
    if (init) init_pending_.insert(cmd);
    if (!timer_on(kTimerPump)) start_timer(kTimerPump, 200);
}

bool Buds::write_raw(const std::vector<uint8_t>& frame) {
    AcquireSRWLockExclusive(&link_->lock);
    bool ok = !link_->closed && link_->sock != INVALID_SOCKET &&
              ::send(link_->sock, reinterpret_cast<const char*>(frame.data()), int(frame.size()), 0) == int(frame.size());
    ReleaseSRWLockExclusive(&link_->lock);
    return ok;
}

void Buds::pump() {
    if (!link_ || queue_.empty()) {
        stop_timer(kTimerPump);
        return;
    }
    if (!write_raw(queue_.front())) {
        stop_timer(kTimerPump);
        drop_link("Lost the control channel while sending");
        return;
    }
    queue_.pop_front();
    if (queue_.empty()) stop_timer(kTimerPump);
}

void Buds::write_now(const std::vector<uint8_t>& frame) {
    if (link_ && !write_raw(frame)) drop_link("Lost the control channel while sending");
}

void Buds::on_data(unsigned gen, const std::vector<uint8_t>& bytes) {
    if (gen != gen_ || !link_) return;
    last_rx_ = GetTickCount64();
    deframer_.feed(bytes.data(), bytes.size());
    while (auto m = deframer_.next()) {
        handle(*m);
        if (!link_) return;
    }
}

void Buds::handle(const proto::Message& m) {
    if (m.cmd & proto::kReplyBit) {
        uint16_t base = m.cmd & ~proto::kReplyBit;
        handle_reply(base, m.payload);
        init_pending_.erase(base);
        if (!st_.initialized && init_pending_.empty()) finish_init();
    } else if (m.cmd == proto::kPush) {
        handle_push(m);
    } else if ((m.cmd >> 8) == 0x05) {
        handle_bud_request(m);
    }
    notify();
}

void Buds::handle_reply(uint16_t base, const std::vector<uint8_t>& p) {
    uint8_t status = p.empty() ? 0xFF : p[0];
    switch (base) {
    case proto::kBitmap:
        if (status == 0) {
            st_.bitmap.assign(p.begin() + 1, p.end());
            enqueue_reads();
        }
        break;
    case proto::kEventsOffered:
        if (status == 0 && p.size() >= 2) {
            size_t count = std::min<size_t>(p[1], p.size() - 2);
            offered_.assign(p.begin() + 2, p.begin() + 2 + long(count));
            subscribe();
        }
        break;
    case proto::kFirmware:
        if (auto fw = proto::parse_firmware(p)) st_.firmware = *fw;
        break;
    case proto::kBattery:
        if (status == 0) {
            st_.battery = {};
            proto::parse_battery(p.data() + 1, p.size() - 1, st_.battery);
        }
        break;
    case proto::kEq:
        if (status == 0 && p.size() >= 2) st_.eq = p[1];
        break;
    case proto::kFeatures:
        if (status == 0 && p.size() >= 2) {
            for (size_t i = 2; i + 1 < p.size(); i += 2)
                if (p[i] == proto::kFeatureGameMode) st_.game_mode = p[i + 1];
        }
        break;
    case proto::kGestures:
        if (status == 0) {
            auto table = proto::parse_gestures(p);
            if (!table.empty()) st_.gestures = std::move(table);
        }
        break;
    case proto::kEqSet:
        if (status != 0) fail("The earbuds refused the sound profile change (" + hex_byte(status) + ")");
        send(proto::kEq);
        break;
    case proto::kFeatureSet:
        if (status != 0) fail("The earbuds refused the game mode change (" + hex_byte(status) + ")");
        send(proto::kFeatures, {0x02, 0x05, proto::kFeatureGameMode});
        break;
    case proto::kGesturesSet:
        if (status != 0) fail("The earbuds refused the touch control change (" + hex_byte(status) + ")");
        send(proto::kGestures, {0x02, 0x03, 0x01});
        break;
    case proto::kFind:
        if (status != 0) {
            st_.finding = false;
            fail("The earbuds refused to ring (" + hex_byte(status) + ")");
        }
        break;
    default:
        break;
    }
}

void Buds::handle_push(const proto::Message& m) {
    const auto& p = m.payload;
    if (p.empty()) return;
    switch (p[0]) {
    case proto::kPushBattery:
        proto::parse_battery(p.data() + 1, p.size() - 1, st_.battery);
        break;
    case proto::kPushGameMode:
        if (p.size() >= 2) st_.game_mode = p.back();
        break;
    case proto::kPushDevicesInfo:
        write_now(proto::encode(m.cmd | proto::kReplyBit, m.seq, {0x00, proto::kPushDevicesInfo}));
        break;
    case proto::kPushDiagnostic: {
        std::vector<uint8_t> answer = {0x00, proto::kPushDiagnostic};
        std::string json(p.begin() + 1, p.end());
        size_t at = json.find("\"cmd\"");
        if (at != std::string::npos) {
            size_t colon = json.find(':', at);
            size_t end = json.find_first_of(",}", colon);
            if (colon != std::string::npos && end != std::string::npos) {
                std::string answer_json = "{\"cmd\":" + json.substr(colon + 1, end - colon - 1) + "}";
                answer.insert(answer.end(), answer_json.begin(), answer_json.end());
            }
        }
        write_now(proto::encode(m.cmd | proto::kReplyBit, m.seq, answer));
        break;
    }
    default:
        break;
    }
}

void Buds::handle_bud_request(const proto::Message& m) {
    std::vector<uint8_t> answer;
    switch (m.cmd) {
    case proto::kBudTimeRequest: {
        uint32_t now = uint32_t(std::time(nullptr));
        answer = {0x00, uint8_t(now), uint8_t(now >> 8), uint8_t(now >> 16), uint8_t(now >> 24)};
        break;
    }
    case proto::kBudEqChanged:
        if (!m.payload.empty()) st_.eq = m.payload[0];
        return;
    case proto::kBudForegroundApp:
        answer = {0x00, 0x01, 0x00};
        break;
    case proto::kBudAppCapabilities:
        answer = {0x00, 0x00, 0x00};
        break;
    case proto::kBudSpatialMode:
        answer = {0x01};
        break;
    default:
        return;
    }
    write_now(proto::encode(m.cmd | proto::kReplyBit, m.seq, answer));
}

void Buds::enqueue_reads() {
    if (supports(proto::kFirmware)) send(proto::kFirmware, {}, true);
    send(proto::kBattery, {}, true);
    if (supports(proto::kEq)) send(proto::kEq, {}, true);
    if (supports(proto::kFeatureSet)) send(proto::kFeatures, {0x02, 0x05, proto::kFeatureGameMode}, true);
    if (supports(proto::kGestures)) send(proto::kGestures, {0x02, 0x03, 0x01}, true);
}

void Buds::subscribe() {
    std::vector<uint8_t> codes;
    for (uint8_t want : {proto::kPushBattery, proto::kPushWearing, proto::kPushGameMode})
        if (std::find(offered_.begin(), offered_.end(), want) != offered_.end()) codes.push_back(want);
    if (codes.empty()) return;
    if (supports(proto::kSubscribe)) {
        std::vector<uint8_t> payload = {uint8_t(codes.size())};
        payload.insert(payload.end(), codes.begin(), codes.end());
        send(proto::kSubscribe, payload);
    } else {
        for (uint8_t c : codes) send(proto::kEventRegister, {c});
    }
}

void Buds::finish_init() {
    if (!link_ || st_.initialized) return;
    st_.initialized = true;
    stop_timer(kTimerInit);
    notify();
}

bool Buds::set_eq(int id) {
    if (!ready() || !supports(proto::kEqSet)) return false;
    send(proto::kEqSet, {uint8_t(id)});
    return true;
}

bool Buds::set_game_mode(bool on) {
    if (!ready() || !supports(proto::kFeatureSet)) return false;
    send(proto::kFeatureSet, {proto::kFeatureGameMode, uint8_t(on ? 1 : 0)});
    return true;
}

bool Buds::set_find(bool on) {
    if (!ready() || !supports(proto::kFind)) return false;
    send(proto::kFind, {uint8_t(on ? 1 : 0)});
    st_.finding = on;
    stop_timer(kTimerFind);
    if (on) start_timer(kTimerFind, 60000);
    notify();
    return true;
}

bool Buds::set_gesture(const std::vector<uint8_t>& devs, uint8_t act, uint8_t fn) {
    if (!ready() || !supports(proto::kGesturesSet) || st_.gestures.empty()) return false;
    // Only the changed entries: these buds answer a whole-table write with 0x01 and apply it partly.
    std::vector<proto::GestureEntry> changes;
    for (auto e : st_.gestures) {
        if (e.act == act && std::find(devs.begin(), devs.end(), e.dev) != devs.end()) {
            e.fn = fn;
            changes.push_back(e);
        }
    }
    if (changes.empty()) return false;
    send(proto::kGesturesSet, proto::encode_gestures(changes));
    return true;
}

void Buds::refresh() {
    if (ready()) send(proto::kBattery);
}

void Buds::set_audio(bool connect) {
    HWND hwnd = hwnd_;
    std::string name = st_.name;
    bool started = util::run_detached([hwnd, name, connect] {
        std::string err;
        bool ok = bt::set_audio_connected(name, connect, &err);
        auto* r = new AudioResult{connect, ok, err};
        if (!PostMessageW(hwnd, kMsgAudio, 0, reinterpret_cast<LPARAM>(r))) delete r;
    });
    if (!started) {
        fail("Bluetooth: could not start a thread");
        notify();
    }
}

void Buds::bluetooth_connect() {
    if (st_.link == LinkState::NoDevice) return;
    stop_timer(kTimerDisconnect);
    st_.disconnecting = false;
    if (st_.bt_connected && !link_ && !connecting_ && !timer_on(kTimerRetry)) start_connect();
    set_audio(true);
    notify();
}

void Buds::bluetooth_disconnect() {
    if (st_.link == LinkState::NoDevice) return;
    // Our RFCOMM channel alone keeps the Bluetooth link up, so close it and hold off reopening it
    // until Windows reports the earbuds gone.
    st_.disconnecting = true;
    close_link();
    stop_timer(kTimerRetry);
    st_.link = LinkState::Disconnected;
    start_timer(kTimerDisconnect, 20000);
    set_audio(false);
    notify();
}
