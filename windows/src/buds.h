#pragma once

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "protocol.h"

enum class LinkState { NoDevice, Disconnected, Connecting, Connected };

struct BudsState {
    LinkState link = LinkState::NoDevice;
    std::string name;
    std::string address;
    bool radio = true;           // a Bluetooth adapter is present and on
    bool bt_connected = false;   // Bluetooth link, as Windows reports it
    bool disconnecting = false;  // we asked Windows to disconnect and are waiting for it
    bool initialized = false;    // control channel open and settings read
    proto::Battery battery;
    proto::Firmware firmware;
    int eq = -1;
    int game_mode = -1;
    bool finding = false;
    std::vector<proto::GestureEntry> gestures;
    std::vector<uint8_t> bitmap;
    std::string last_error;
    unsigned error_serial = 0;
};

// The earbuds: finds them among the paired devices, keeps the RFCOMM control channel open while they are
// connected and mirrors their settings in state(). Lives on the UI thread; worker threads report back
// through a message-only window.
class Buds {
public:
    explicit Buds(std::string preferred_address);
    ~Buds();
    Buds(const Buds&) = delete;
    Buds& operator=(const Buds&) = delete;

    void start();
    const BudsState& state() const { return st_; }
    bool ready() const { return link_ && st_.initialized; }
    bool supports(uint16_t cmd) const { return proto::bitmap_allows(st_.bitmap, cmd); }
    void add_listener(std::function<void()> fn) { listeners_.push_back(std::move(fn)); }

    bool set_eq(int id);
    bool set_game_mode(bool on);
    bool set_find(bool on);
    bool set_gesture(const std::vector<uint8_t>& devs, uint8_t act, uint8_t fn);
    void refresh();
    void bluetooth_connect();
    void bluetooth_disconnect();

#ifdef NORDBUDS_TEST_HOOKS
    void set_test_state(const BudsState& st) {
        st_ = st;
        if (!link_) link_ = std::make_shared<Link>();
    }
#endif

private:
    // The socket, shared with its reader thread. The reader closes it; the UI thread only shuts it down.
    struct Link {
        SRWLOCK lock = SRWLOCK_INIT;
        SOCKET sock = INVALID_SOCKET;
        bool closed = false;
    };
    struct Pick {
        bool found = false;
        std::string name, address;
        uint64_t addr = 0;
        bool connected = false;
        bool radio = true;
        bool operator==(const Pick&) const = default;
    };

    static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void on_timer(UINT_PTR id);
    void start_timer(UINT_PTR id, UINT ms);
    void stop_timer(UINT_PTR id);
    bool timer_on(UINT_PTR id) const { return timers_.count(id) != 0; }

    void notify();
    void fail(const std::string& message);
    void monitor_loop();
    void on_devices(const Pick& pick);
    void start_connect();
    void on_connected(unsigned gen, SOCKET sock, const std::string& err);
    void schedule_retry();
    void open_link(SOCKET sock);
    void close_link();
    void drop_link(const std::string& reason);
    void send(uint16_t cmd, std::vector<uint8_t> payload = {}, bool init = false);
    bool write_raw(const std::vector<uint8_t>& frame);
    void write_now(const std::vector<uint8_t>& frame);
    void pump();
    void on_data(unsigned gen, const std::vector<uint8_t>& bytes);
    void handle(const proto::Message& m);
    void handle_reply(uint16_t base, const std::vector<uint8_t>& p);
    void handle_push(const proto::Message& m);
    void handle_bud_request(const proto::Message& m);
    void enqueue_reads();
    void subscribe();
    void finish_init();
    void set_audio(bool connect);

    std::string preferred_;
    BudsState st_;
    std::vector<std::function<void()>> listeners_;

    HWND hwnd_ = nullptr;
    HANDLE monitor_ = nullptr;
    HANDLE wake_ = nullptr;
    std::atomic<bool> stopping_{false};
    uint64_t addr_ = 0;

    std::shared_ptr<Link> link_;
    bool connecting_ = false;
    unsigned gen_ = 0;
    int retries_ = 0;
    uint8_t seq_ = 1;
    proto::Deframer deframer_;
    std::deque<std::vector<uint8_t>> queue_;
    std::set<uint16_t> init_pending_;
    std::vector<uint8_t> offered_;
    ULONGLONG last_rx_ = 0;  // GetTickCount64() of the last bytes from the buds
    std::set<UINT_PTR> timers_;
};
