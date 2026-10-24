#pragma once

#include <gio/gio.h>

#include <deque>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "protocol.h"

enum class LinkState { NoDevice, Disconnected, Connecting, Connected };

struct BudsState {
    LinkState link = LinkState::NoDevice;
    std::string name;
    std::string address;
    bool bt_connected = false;  // Bluetooth audio link, as BlueZ reports it
    bool initialized = false;   // control channel open and settings read
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

class Buds {
public:
    explicit Buds(std::string preferred_address);
    ~Buds();
    Buds(const Buds&) = delete;
    Buds& operator=(const Buds&) = delete;

    void start();
    const BudsState& state() const { return st_; }
    bool ready() const { return fd_ >= 0 && st_.initialized; }
    bool supports(uint16_t cmd) const { return proto::bitmap_allows(st_.bitmap, cmd); }
    void add_listener(std::function<void()> fn) { listeners_.push_back(std::move(fn)); }

    bool set_eq(int id);
    bool set_game_mode(bool on);
    bool set_find(bool on);
    bool set_gesture(const std::vector<uint8_t>& devs, uint8_t act, uint8_t fn);
    void refresh();
    void bluetooth_connect();
    void bluetooth_disconnect();

private:
    void notify();
    void fail(const std::string& message);
    void pick_device();
    void sync_device();
    void start_connect();
    void on_connect_done(GAsyncResult* res);
    void schedule_retry();
    void open_link(int fd);
    void close_link();
    void drop_link(const std::string& reason);
    void send(uint16_t cmd, std::vector<uint8_t> payload = {}, bool init = false);
    void write_now(const std::vector<uint8_t>& frame);
    bool pump();
    void read_ready();
    void handle(const proto::Message& m);
    void handle_reply(uint16_t base, const std::vector<uint8_t>& p);
    void handle_push(const proto::Message& m);
    void handle_bud_request(const proto::Message& m);
    void enqueue_reads();
    void subscribe();
    void finish_init();
    void call_device(const char* method);

    static gboolean on_fd(gint fd, GIOCondition cond, gpointer self);

    std::string preferred_;
    BudsState st_;
    std::vector<std::function<void()>> listeners_;

    GDBusObjectManager* manager_ = nullptr;
    GDBusProxy* device_ = nullptr;

    int fd_ = -1;
    bool connecting_ = false;
    unsigned gen_ = 0;
    int retries_ = 0;
    uint8_t seq_ = 1;
    proto::Deframer deframer_;
    std::deque<std::vector<uint8_t>> queue_;
    std::set<uint16_t> init_pending_;
    std::vector<uint8_t> offered_;
    gint64 last_rx_ = 0;  // monotonic time of the last bytes from the buds

    guint watch_id_ = 0;
    guint pump_id_ = 0;
    guint retry_id_ = 0;
    guint init_timeout_id_ = 0;
    guint poll_id_ = 0;
    guint find_stop_id_ = 0;
};
