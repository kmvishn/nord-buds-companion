#include "buds.h"

#include <glib-unix.h>
#include <strings.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <ctime>

#include "bt.h"

namespace {

const char* const kVendorUuids[] = {
    "0000079a-d102-11e1-9b23-00025b00a5a5",
    "00001107-d102-11e1-9b23-00025b00a5a5",
};

// Same services as kVendorUuids, in the order the vendor app tries them.
const uint8_t kVendorUuidBytes[2][16] = {
    {0x00, 0x00, 0x07, 0x9a, 0xd1, 0x02, 0x11, 0xe1, 0x9b, 0x23, 0x00, 0x02, 0x5b, 0x00, 0xa5, 0xa5},
    {0x00, 0x00, 0x11, 0x07, 0xd1, 0x02, 0x11, 0xe1, 0x9b, 0x23, 0x00, 0x02, 0x5b, 0x00, 0xa5, 0xa5},
};

const int kRetryDelays[] = {2, 5, 10, 30};

std::string prop_string(GDBusProxy* p, const char* name) {
    GVariant* v = g_dbus_proxy_get_cached_property(p, name);
    if (!v) return {};
    std::string s = g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) ? g_variant_get_string(v, nullptr) : "";
    g_variant_unref(v);
    return s;
}

bool prop_bool(GDBusProxy* p, const char* name) {
    GVariant* v = g_dbus_proxy_get_cached_property(p, name);
    if (!v) return false;
    bool b = g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(v);
    g_variant_unref(v);
    return b;
}

bool has_vendor_uuid(GDBusProxy* p) {
    GVariant* v = g_dbus_proxy_get_cached_property(p, "UUIDs");
    if (!v) return false;
    bool found = false;
    gsize n = 0;
    const gchar** list = g_variant_get_strv(v, &n);
    for (gsize i = 0; i < n && !found; i++)
        for (const char* u : kVendorUuids)
            if (g_ascii_strcasecmp(list[i], u) == 0) found = true;
    g_free(list);
    g_variant_unref(v);
    return found;
}

struct ConnectJob {
    std::string address;
    unsigned gen;
};

void connect_thread(GTask* task, gpointer, gpointer data, GCancellable*) {
    auto* job = static_cast<ConnectJob*>(data);
    std::string err, last = "control service not found";
    for (const auto& uuid : kVendorUuidBytes) {
        int channel = bt::sdp_rfcomm_channel(job->address, uuid, &err);
        if (channel < 0) {
            if (err != "control service not offered") last = err;
            continue;
        }
        int fd = bt::rfcomm_connect(job->address, channel, &err);
        if (fd >= 0) {
            g_task_return_int(task, fd);
            return;
        }
        last = err;
    }
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", last.c_str());
}

std::string hex_byte(uint8_t b) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "0x%02X", b);
    return buf;
}

}  // namespace

Buds::Buds(std::string preferred_address) : preferred_(std::move(preferred_address)) {}

Buds::~Buds() {
    close_link();
    if (retry_id_) g_source_remove(retry_id_);
    if (device_) g_object_unref(device_);
    if (manager_) g_object_unref(manager_);
}

void Buds::notify() {
    for (auto& fn : listeners_) fn();
}

void Buds::fail(const std::string& message) {
    st_.last_error = message;
    st_.error_serial++;
}

void Buds::start() {
    GError* err = nullptr;
    manager_ = g_dbus_object_manager_client_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, "org.bluez", "/",
        nullptr, nullptr, nullptr, nullptr, &err);
    if (!manager_) {
        fail(std::string("Bluetooth service unavailable: ") + err->message);
        g_error_free(err);
        notify();
        return;
    }
    g_signal_connect(manager_, "object-added",
                     G_CALLBACK(+[](GDBusObjectManager*, GDBusObject*, gpointer self) {
                         static_cast<Buds*>(self)->pick_device();
                     }),
                     this);
    g_signal_connect(manager_, "object-removed",
                     G_CALLBACK(+[](GDBusObjectManager*, GDBusObject* obj, gpointer data) {
                         auto* self = static_cast<Buds*>(data);
                         if (self->device_ && g_strcmp0(g_dbus_object_get_object_path(obj),
                                                        g_dbus_proxy_get_object_path(self->device_)) == 0) {
                             self->close_link();
                             g_clear_object(&self->device_);
                         }
                         self->pick_device();
                     }),
                     this);
    g_signal_connect(manager_, "interface-proxy-properties-changed",
                     G_CALLBACK(+[](GDBusObjectManagerClient*, GDBusObjectProxy*, GDBusProxy* iface,
                                    GVariant*, GStrv, gpointer data) {
                         auto* self = static_cast<Buds*>(data);
                         if (iface == self->device_) {
                             self->sync_device();
                         } else if (g_strcmp0(g_dbus_proxy_get_interface_name(iface), "org.bluez.Device1") == 0 &&
                                    !self->st_.bt_connected) {
                             self->pick_device();
                         }
                     }),
                     this);
    pick_device();
}

void Buds::pick_device() {
    if (!manager_) return;
    GDBusProxy* best = nullptr;
    int best_score = -1;
    GList* objects = g_dbus_object_manager_get_objects(manager_);
    for (GList* l = objects; l; l = l->next) {
        GDBusInterface* iface = g_dbus_object_get_interface(G_DBUS_OBJECT(l->data), "org.bluez.Device1");
        if (!iface) continue;
        GDBusProxy* p = G_DBUS_PROXY(iface);
        std::string address = prop_string(p, "Address");
        std::string name = prop_string(p, "Name");
        int score = -1;
        if (!preferred_.empty()) {
            if (strcasecmp(address.c_str(), preferred_.c_str()) == 0) score = 1;
        } else if (prop_bool(p, "Paired") &&
                   (has_vendor_uuid(p) || name.find("Nord Buds") != std::string::npos)) {
            score = 1;
        }
        if (score >= 0 && prop_bool(p, "Connected")) score += 2;
        if (score > best_score) {
            if (best) g_object_unref(best);
            best = p;
            best_score = score;
        } else {
            g_object_unref(iface);
        }
    }
    g_list_free_full(objects, g_object_unref);

    if (best && device_ && g_strcmp0(g_dbus_proxy_get_object_path(best), g_dbus_proxy_get_object_path(device_)) == 0) {
        g_object_unref(best);
    } else if (best) {
        close_link();
        if (device_) g_object_unref(device_);
        device_ = best;
        st_.bt_connected = false;
    }
    sync_device();
}

void Buds::sync_device() {
    if (!device_) {
        st_.link = LinkState::NoDevice;
        st_.name.clear();
        st_.address.clear();
        st_.bt_connected = false;
        notify();
        return;
    }
    std::string alias = prop_string(device_, "Alias");
    st_.name = alias.empty() ? prop_string(device_, "Name") : alias;
    st_.address = prop_string(device_, "Address");
    st_.bt_connected = prop_bool(device_, "Connected");

    if (st_.bt_connected) {
        if (fd_ < 0 && !connecting_ && !retry_id_) start_connect();
    } else {
        close_link();
        if (retry_id_) {
            g_source_remove(retry_id_);
            retry_id_ = 0;
        }
        retries_ = 0;
        st_.link = LinkState::Disconnected;
    }
    notify();
}

void Buds::start_connect() {
    connecting_ = true;
    st_.link = LinkState::Connecting;
    GTask* task = g_task_new(nullptr, nullptr,
                             +[](GObject*, GAsyncResult* res, gpointer self) {
                                 static_cast<Buds*>(self)->on_connect_done(res);
                             },
                             this);
    g_task_set_task_data(task, new ConnectJob{st_.address, ++gen_},
                         +[](gpointer p) { delete static_cast<ConnectJob*>(p); });
    g_task_run_in_thread(task, connect_thread);
    g_object_unref(task);
}

void Buds::on_connect_done(GAsyncResult* res) {
    auto* job = static_cast<ConnectJob*>(g_task_get_task_data(G_TASK(res)));
    GError* err = nullptr;
    gssize fd = g_task_propagate_int(G_TASK(res), &err);
    if (job->gen != gen_) {
        if (fd >= 0) close(int(fd));
        if (err) g_error_free(err);
        return;
    }
    connecting_ = false;
    if (fd < 0) {
        fail(std::string("Could not open the control channel: ") + (err ? err->message : "unknown error"));
        if (err) g_error_free(err);
        if (st_.bt_connected) schedule_retry();
        notify();
        return;
    }
    if (!st_.bt_connected) {
        close(int(fd));
        return;
    }
    open_link(int(fd));
}

void Buds::schedule_retry() {
    if (retry_id_) return;
    st_.link = LinkState::Connecting;
    int delay = kRetryDelays[std::min<size_t>(size_t(retries_), std::size(kRetryDelays) - 1)];
    retries_++;
    retry_id_ = g_timeout_add_seconds(guint(delay),
                                      +[](gpointer data) -> gboolean {
                                          auto* self = static_cast<Buds*>(data);
                                          self->retry_id_ = 0;
                                          if (self->st_.bt_connected && self->fd_ < 0 && !self->connecting_)
                                              self->start_connect();
                                          self->notify();
                                          return G_SOURCE_REMOVE;
                                      },
                                      this);
}

void Buds::open_link(int fd) {
    fd_ = fd;
    retries_ = 0;
    st_.last_error.clear();
    st_.link = LinkState::Connected;
    st_.initialized = false;
    deframer_ = proto::Deframer();
    last_rx_ = g_get_monotonic_time();
    watch_id_ = g_unix_fd_add(fd_, GIOCondition(G_IO_IN | G_IO_HUP | G_IO_ERR), on_fd, this);

    send(proto::kBitmap, {}, true);
    send(proto::kEventsOffered, {}, true);
    init_timeout_id_ = g_timeout_add_seconds(6,
                                             +[](gpointer data) -> gboolean {
                                                 auto* self = static_cast<Buds*>(data);
                                                 self->init_timeout_id_ = 0;
                                                 self->finish_init();
                                                 return G_SOURCE_REMOVE;
                                             },
                                             this);
    poll_id_ = g_timeout_add_seconds(60,
                                     +[](gpointer data) -> gboolean {
                                         auto* self = static_cast<Buds*>(data);
                                         if (!self->ready()) return G_SOURCE_CONTINUE;
                                         // Two unanswered polls: the link is dead even if the socket never said so.
                                         if (g_get_monotonic_time() - self->last_rx_ > 150 * G_USEC_PER_SEC) {
                                             self->poll_id_ = 0;
                                             self->drop_link("The earbuds stopped responding");
                                             return G_SOURCE_REMOVE;
                                         }
                                         self->send(proto::kBattery);
                                         return G_SOURCE_CONTINUE;
                                     },
                                     this);
    notify();
}

void Buds::close_link() {
    gen_++;
    connecting_ = false;
    for (guint* id : {&watch_id_, &pump_id_, &init_timeout_id_, &poll_id_, &find_stop_id_}) {
        if (*id) g_source_remove(*id);
        *id = 0;
    }
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
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
    if (st_.bt_connected) {
        fail(reason);
        schedule_retry();
    } else {
        st_.link = LinkState::Disconnected;
    }
    notify();
}

void Buds::send(uint16_t cmd, std::vector<uint8_t> payload, bool init) {
    if (fd_ < 0) return;
    queue_.push_back(proto::encode(cmd, seq_, payload));
    seq_ = seq_ >= 0xFE ? 1 : seq_ + 1;
    if (init) init_pending_.insert(cmd);
    if (!pump_id_) {
        pump_id_ = g_timeout_add(200,
                                 +[](gpointer data) -> gboolean {
                                     return static_cast<Buds*>(data)->pump() ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
                                 },
                                 this);
    }
}

bool Buds::pump() {
    if (fd_ < 0 || queue_.empty()) {
        pump_id_ = 0;
        return false;
    }
    const auto& frame = queue_.front();
    ssize_t n = write(fd_, frame.data(), frame.size());
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
    if (n != ssize_t(frame.size())) {
        pump_id_ = 0;
        drop_link("Lost the control channel while sending");
        return false;
    }
    queue_.pop_front();
    if (queue_.empty()) {
        pump_id_ = 0;
        return false;
    }
    return true;
}

void Buds::write_now(const std::vector<uint8_t>& frame) {
    if (fd_ >= 0 && write(fd_, frame.data(), frame.size()) < 0 && errno != EAGAIN)
        drop_link("Lost the control channel while sending");
}

gboolean Buds::on_fd(gint, GIOCondition cond, gpointer data) {
    auto* self = static_cast<Buds*>(data);
    if (cond & G_IO_IN) self->read_ready();
    if (self->fd_ < 0) return G_SOURCE_REMOVE;  // read_ready() dropped the link and removed this source
    if (cond & (G_IO_HUP | G_IO_ERR)) {
        self->watch_id_ = 0;
        self->drop_link("The earbuds closed the control channel");
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void Buds::read_ready() {
    uint8_t buf[2048];
    while (fd_ >= 0) {
        ssize_t n = read(fd_, buf, sizeof buf);
        if (n > 0) {
            last_rx_ = g_get_monotonic_time();
            deframer_.feed(buf, size_t(n));
            while (auto m = deframer_.next()) {
                handle(*m);
                if (fd_ < 0) return;
            }
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        drop_link(n == 0 ? "The earbuds closed the control channel" : "Control channel read error");
        return;
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
    if (fd_ < 0 || st_.initialized) return;
    st_.initialized = true;
    if (init_timeout_id_) {
        g_source_remove(init_timeout_id_);
        init_timeout_id_ = 0;
    }
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
    if (find_stop_id_) {
        g_source_remove(find_stop_id_);
        find_stop_id_ = 0;
    }
    if (on) {
        find_stop_id_ = g_timeout_add_seconds(60,
                                              +[](gpointer data) -> gboolean {
                                                  auto* self = static_cast<Buds*>(data);
                                                  self->find_stop_id_ = 0;
                                                  self->set_find(false);
                                                  return G_SOURCE_REMOVE;
                                              },
                                              this);
    }
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

void Buds::call_device(const char* method) {
    if (!device_) return;
    g_dbus_proxy_call(device_, method, nullptr, G_DBUS_CALL_FLAGS_NONE, 30000, nullptr,
                      +[](GObject* src, GAsyncResult* res, gpointer data) {
                          GError* err = nullptr;
                          GVariant* r = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &err);
                          if (r) g_variant_unref(r);
                          if (err) {
                              auto* self = static_cast<Buds*>(data);
                              self->fail(std::string("Bluetooth: ") + err->message);
                              g_error_free(err);
                              self->notify();
                          }
                      },
                      this);
}

void Buds::bluetooth_connect() {
    call_device("Connect");
}

void Buds::bluetooth_disconnect() {
    call_device("Disconnect");
}
