#include "indicator.h"

#include <unistd.h>

#include <cstdio>

#include "buds.h"
#include "model.h"
#include "settings.h"

namespace {

constexpr const char* kItemPath = "/StatusNotifierItem";
constexpr const char* kMenuPath = "/MenuBar";
constexpr const char* kFallbackIcon = "audio-headphones-symbolic";
constexpr const char* kCustomIcon = "nordbuds-symbolic";

bool custom_icon_installed() {
    static const bool found = g_file_test(NORDBUDS_ICON_DIR "/nordbuds-symbolic.svg", G_FILE_TEST_EXISTS);
    return found;
}

const char* icon_name() {
    return custom_icon_installed() ? kCustomIcon : kFallbackIcon;
}

const char* icon_path() {
    return custom_icon_installed() ? NORDBUDS_ICON_DIR : "";
}

const char* label_guide(const std::string& style) {
    if (style == "compact") return "100%";
    if (style == "buds") return "L 100%  R 100%";
    if (style == "all") return "L 100%  R 100%  C 100%";
    return "";
}

constexpr const char* kIntrospection = R"XML(
<node>
  <interface name="org.kde.StatusNotifierItem">
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="WindowId" type="i" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconThemePath" type="s" access="read"/>
    <property name="IconPixmap" type="a(iiay)" access="read"/>
    <property name="OverlayIconName" type="s" access="read"/>
    <property name="OverlayIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionIconName" type="s" access="read"/>
    <property name="AttentionIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionMovieName" type="s" access="read"/>
    <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <property name="XAyatanaLabel" type="s" access="read"/>
    <property name="XAyatanaLabelGuide" type="s" access="read"/>
    <property name="XAyatanaOrderingIndex" type="u" access="read"/>
    <method name="ContextMenu"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="Activate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="SecondaryActivate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
    <method name="Scroll"><arg name="delta" type="i" direction="in"/><arg name="orientation" type="s" direction="in"/></method>
    <signal name="NewTitle"/>
    <signal name="NewIcon"/>
    <signal name="NewAttentionIcon"/>
    <signal name="NewOverlayIcon"/>
    <signal name="NewToolTip"/>
    <signal name="NewStatus"><arg name="status" type="s"/></signal>
    <signal name="XAyatanaNewLabel"><arg name="label" type="s"/><arg name="guide" type="s"/></signal>
  </interface>
  <interface name="com.canonical.dbusmenu">
    <property name="Version" type="u" access="read"/>
    <property name="TextDirection" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconThemePath" type="as" access="read"/>
    <method name="GetLayout">
      <arg type="i" name="parentId" direction="in"/>
      <arg type="i" name="recursionDepth" direction="in"/>
      <arg type="as" name="propertyNames" direction="in"/>
      <arg type="u" name="revision" direction="out"/>
      <arg type="(ia{sv}av)" name="layout" direction="out"/>
    </method>
    <method name="GetGroupProperties">
      <arg type="ai" name="ids" direction="in"/>
      <arg type="as" name="propertyNames" direction="in"/>
      <arg type="a(ia{sv})" name="properties" direction="out"/>
    </method>
    <method name="GetProperty">
      <arg type="i" name="id" direction="in"/>
      <arg type="s" name="name" direction="in"/>
      <arg type="v" name="value" direction="out"/>
    </method>
    <method name="Event">
      <arg type="i" name="id" direction="in"/>
      <arg type="s" name="eventId" direction="in"/>
      <arg type="v" name="data" direction="in"/>
      <arg type="u" name="timestamp" direction="in"/>
    </method>
    <method name="EventGroup">
      <arg type="a(isvu)" name="events" direction="in"/>
      <arg type="ai" name="idErrors" direction="out"/>
    </method>
    <method name="AboutToShow">
      <arg type="i" name="id" direction="in"/>
      <arg type="b" name="needUpdate" direction="out"/>
    </method>
    <method name="AboutToShowGroup">
      <arg type="ai" name="ids" direction="in"/>
      <arg type="ai" name="updatesNeeded" direction="out"/>
      <arg type="ai" name="idErrors" direction="out"/>
    </method>
    <signal name="ItemsPropertiesUpdated">
      <arg type="a(ia{sv})" name="updatedProps"/>
      <arg type="a(ias)" name="removedProps"/>
    </signal>
    <signal name="LayoutUpdated"><arg type="u" name="revision"/><arg type="i" name="parent"/></signal>
    <signal name="ItemActivationRequested"><arg type="i" name="id"/><arg type="u" name="timestamp"/></signal>
  </interface>
</node>
)XML";

GVariant* empty_pixmaps() {
    return g_variant_new_array(G_VARIANT_TYPE("(iiay)"), nullptr, 0);
}

std::string percent(const proto::BatteryPart& b) {
    if (b.level < 0) return "–";
    return std::to_string(b.level) + "%";
}

std::string battery_line(const char* name, const proto::BatteryPart& b, const char* missing) {
    std::string s = std::string(name) + ": ";
    if (b.level < 0) return s + missing;
    s += std::to_string(b.level) + "%";
    if (b.charging) s += "  (charging)";
    return s;
}

void sni_method_cb(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method,
                   GVariant*, GDBusMethodInvocation* inv, gpointer self) {
    static_cast<Indicator*>(self)->sni_method(method, inv);
}

GVariant* sni_property_cb(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* name,
                          GError**, gpointer self) {
    return static_cast<Indicator*>(self)->sni_property(name);
}

void menu_method_cb(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method,
                    GVariant* params, GDBusMethodInvocation* inv, gpointer self) {
    static_cast<Indicator*>(self)->menu_method(method, params, inv);
}

GVariant* menu_property_cb(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* name,
                           GError**, gpointer self) {
    return static_cast<Indicator*>(self)->menu_property(name);
}

const GDBusInterfaceVTable kSniVtable = {sni_method_cb, sni_property_cb, nullptr, {}};
const GDBusInterfaceVTable kMenuVtable = {menu_method_cb, menu_property_cb, nullptr, {}};

}  // namespace

Indicator::Indicator(Buds& buds, Actions actions) : buds_(buds), actions_(std::move(actions)) {
    bus_name_ = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";
}

Indicator::~Indicator() {
    if (watch_id_) g_bus_unwatch_name(watch_id_);
    if (own_id_) g_bus_unown_name(own_id_);
    if (bus_ && sni_reg_) g_dbus_connection_unregister_object(bus_, sni_reg_);
    if (bus_ && menu_reg_) g_dbus_connection_unregister_object(bus_, menu_reg_);
    if (node_) g_dbus_node_info_unref(node_);
    if (bus_) g_object_unref(bus_);
}

void Indicator::start() {
    GError* err = nullptr;
    bus_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!bus_) {
        g_warning("indicator: no session bus: %s", err->message);
        g_error_free(err);
        return;
    }
    node_ = g_dbus_node_info_new_for_xml(kIntrospection, &err);
    if (!node_) {
        g_warning("indicator: %s", err->message);
        g_error_free(err);
        return;
    }
    build_items();
    sni_reg_ = g_dbus_connection_register_object(
        bus_, kItemPath, g_dbus_node_info_lookup_interface(node_, "org.kde.StatusNotifierItem"),
        &kSniVtable, this, nullptr, &err);
    if (!sni_reg_) {
        g_warning("indicator: %s", err->message);
        g_clear_error(&err);
    }
    menu_reg_ = g_dbus_connection_register_object(
        bus_, kMenuPath, g_dbus_node_info_lookup_interface(node_, "com.canonical.dbusmenu"),
        &kMenuVtable, this, nullptr, &err);
    if (!menu_reg_) {
        g_warning("indicator: %s", err->message);
        g_clear_error(&err);
    }

    own_id_ = g_bus_own_name_on_connection(
        bus_, bus_name_.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE,
        +[](GDBusConnection*, const gchar*, gpointer data) {
            auto* self = static_cast<Indicator*>(data);
            self->name_owned_ = true;
            self->try_register();
        },
        nullptr, this, nullptr);
    watch_id_ = g_bus_watch_name_on_connection(
        bus_, "org.kde.StatusNotifierWatcher", G_BUS_NAME_WATCHER_FLAGS_NONE,
        +[](GDBusConnection*, const gchar*, const gchar*, gpointer data) {
            auto* self = static_cast<Indicator*>(data);
            self->watcher_present_ = true;
            self->try_register();
        },
        +[](GDBusConnection*, const gchar*, gpointer data) {
            static_cast<Indicator*>(data)->watcher_present_ = false;
        },
        this, nullptr);
}

void Indicator::try_register() {
    if (!name_owned_ || !watcher_present_) return;
    g_dbus_connection_call(bus_, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher",
                           "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem",
                           g_variant_new("(s)", bus_name_.c_str()), nullptr, G_DBUS_CALL_FLAGS_NONE, -1,
                           nullptr,
                           +[](GObject* src, GAsyncResult* res, gpointer) {
                               GError* err = nullptr;
                               GVariant* r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
                               if (r) g_variant_unref(r);
                               if (err) {
                                   g_warning("indicator: register failed: %s", err->message);
                                   g_error_free(err);
                               }
                           },
                           nullptr);
}

std::string Indicator::label() const {
    return panel_label(buds_.state(), buds_.ready(), Settings::get().panel().label);
}

std::string Indicator::tooltip() const {
    const auto& st = buds_.state();
    if (!buds_.ready()) return st.bt_connected ? "Connecting…" : "Not connected";
    const auto& b = st.battery;
    return "Left " + percent(b.left) + " · Right " + percent(b.right) + " · Case " + percent(b.box);
}

void Indicator::build_items() {
    const auto& st = buds_.state();
    bool ready = buds_.ready();
    std::vector<Item> items;
    // Each entry keeps its own id, so an id never changes meaning between layouts.
    enum : int { kLeftId = 1, kRightId, kCaseId, kStatusId, kSep1 = 10, kSep2, kSep3, kGameId = 20, kFindId,
                 kOpenId = 30, kConnectId, kDisconnectId, kQuitId, kEqBase = 100 };
    auto add = [&](int id, Item item) {
        item.id = id;
        items.push_back(std::move(item));
    };
    auto separator = [&](int id) {
        Item s;
        s.separator = true;
        add(id, s);
    };

    std::string title = st.name.empty() ? "Nord Buds" : st.name;
    if (ready) {
        add(kLeftId, {0, battery_line("Left", st.battery.left, "not reported"), false});
        add(kRightId, {0, battery_line("Right", st.battery.right, "not reported"), false});
        add(kCaseId, {0, battery_line("Case", st.battery.box, "open the lid to read"), false});
    } else if (!st.bluetooth) {
        add(kStatusId, {0, "Bluetooth is off", false});
    } else if (st.link == LinkState::NoDevice) {
        add(kStatusId, {0, "No paired Nord Buds found", false});
    } else if (st.bt_connected) {
        add(kStatusId, {0, title + ": connecting…", false});
    } else {
        add(kStatusId, {0, title + ": not connected", false});
    }

    if (ready) {
        separator(kSep1);
        for (const auto& preset : model::eq_presets()) {
            Item item{0, preset.name, buds_.supports(proto::kEqSet)};
            item.toggle_type = "radio";
            item.toggled = st.eq == preset.id;
            int preset_id = preset.id;
            item.action = [this, preset_id] { buds_.set_eq(preset_id); };
            add(kEqBase + preset_id, item);
        }
        separator(kSep2);
        Item game{0, "Game mode (low latency)", buds_.supports(proto::kFeatureSet)};
        game.toggle_type = "checkmark";
        game.toggled = st.game_mode == 1;
        game.action = [this] { buds_.set_game_mode(buds_.state().game_mode != 1); };
        add(kGameId, game);
        Item find{0, st.finding ? "Stop ringing" : "Ring earbuds", buds_.supports(proto::kFind)};
        find.action = [this] { buds_.set_find(!buds_.state().finding); };
        add(kFindId, find);
    }

    separator(kSep3);
    add(kOpenId, {0, "Open Nord Buds…", true, false, nullptr, false, [this] { actions_.open_window(); }});
    if (st.link != LinkState::NoDevice) {
        if (st.bt_connected)
            add(kDisconnectId, {0, "Disconnect", true, false, nullptr, false, [this] { buds_.bluetooth_disconnect(); }});
        else
            add(kConnectId, {0, "Connect", true, false, nullptr, false, [this] { buds_.bluetooth_connect(); }});
    }
    add(kQuitId, {0, "Quit", true, false, nullptr, false, [this] { actions_.quit(); }});
    items_ = std::move(items);
}

void Indicator::update() {
    build_items();
    if (!bus_) return;
    revision_++;
    g_dbus_connection_emit_signal(bus_, nullptr, kMenuPath, "com.canonical.dbusmenu", "LayoutUpdated",
                                  g_variant_new("(ui)", revision_, 0), nullptr);
    std::string l = label();
    if (l != last_label_) {
        last_label_ = l;
        g_dbus_connection_emit_signal(bus_, nullptr, kItemPath, "org.kde.StatusNotifierItem", "XAyatanaNewLabel",
                                      g_variant_new("(ss)", l.c_str(), label_guide(Settings::get().panel().label)),
                                      nullptr);
    }
    std::string status = status_text();
    if (status != last_status_) {
        last_status_ = status;
        g_dbus_connection_emit_signal(bus_, nullptr, kItemPath, "org.kde.StatusNotifierItem", "NewStatus",
                                      g_variant_new("(s)", status.c_str()), nullptr);
    }
    // Only on real changes: the AppIndicator extension cancels and re-batches refreshes on every signal.
    std::string tip = tooltip();
    if (tip != last_tooltip_) {
        last_tooltip_ = tip;
        g_dbus_connection_emit_signal(bus_, nullptr, kItemPath, "org.kde.StatusNotifierItem", "NewToolTip", nullptr,
                                      nullptr);
    }
    std::string title = buds_.state().name;
    if (title != last_title_) {
        last_title_ = title;
        g_dbus_connection_emit_signal(bus_, nullptr, kItemPath, "org.kde.StatusNotifierItem", "NewTitle", nullptr,
                                      nullptr);
    }
}

GVariant* Indicator::sni_property(const char* name) {
    const auto& st = buds_.state();
    std::string title = st.name.empty() ? "Nord Buds" : st.name;
    std::string n = name;
    if (n == "Category") return g_variant_new_string("Hardware");
    if (n == "Id") return g_variant_new_string("nordbuds");
    if (n == "Title") return g_variant_new_string(title.c_str());
    if (n == "Status") return g_variant_new_string(status_text().c_str());
    if (n == "WindowId") return g_variant_new_int32(0);
    if (n == "IconName") return g_variant_new_string(icon_name());
    if (n == "IconThemePath") return g_variant_new_string(icon_path());
    if (n == "OverlayIconName" || n == "AttentionIconName" || n == "AttentionMovieName")
        return g_variant_new_string("");
    if (n == "IconPixmap" || n == "OverlayIconPixmap" || n == "AttentionIconPixmap") return empty_pixmaps();
    if (n == "ToolTip")
        return g_variant_new("(s@a(iiay)ss)", icon_name(), empty_pixmaps(), title.c_str(), tooltip().c_str());
    if (n == "ItemIsMenu") return g_variant_new_boolean(FALSE);
    if (n == "Menu") return g_variant_new_object_path(kMenuPath);
    if (n == "XAyatanaLabel") return g_variant_new_string(label().c_str());
    if (n == "XAyatanaLabelGuide") return g_variant_new_string(label_guide(Settings::get().panel().label));
    if (n == "XAyatanaOrderingIndex") return g_variant_new_uint32(0);
    return nullptr;
}

GVariant* Indicator::menu_property(const char* name) {
    std::string n = name;
    if (n == "Version") return g_variant_new_uint32(3);
    if (n == "TextDirection") return g_variant_new_string("ltr");
    if (n == "Status") return g_variant_new_string("normal");
    if (n == "IconThemePath") return g_variant_new_strv(nullptr, 0);
    return nullptr;
}

void Indicator::sni_method(const char* method, GDBusMethodInvocation* inv) {
    g_dbus_method_invocation_return_value(inv, nullptr);
    std::string m = method;
    if (m == "Activate" || m == "SecondaryActivate") actions_.open_window();
}

GVariant* Indicator::item_properties(const Item& item) {
    // Always send every property: the AppIndicator extension keeps any property an update leaves out,
    // so a stale label or "enabled" would otherwise survive on a reused id.
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    auto add = [&b](const char* key, GVariant* v) { g_variant_builder_add(&b, "{sv}", key, v); };
    add("type", g_variant_new_string(item.separator ? "separator" : "standard"));
    add("label", g_variant_new_string(item.separator ? "" : item.label.c_str()));
    add("enabled", g_variant_new_boolean(item.separator || item.enabled));
    add("visible", g_variant_new_boolean(TRUE));
    add("toggle-type", g_variant_new_string(item.toggle_type ? item.toggle_type : ""));
    add("toggle-state", g_variant_new_int32(item.toggle_type ? (item.toggled ? 1 : 0) : -1));
    return g_variant_builder_end(&b);
}

GVariant* Indicator::layout(int parent) {
    if (parent != 0) {
        const Item* item = find(parent);
        GVariant* props = item ? item_properties(*item) : g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0);
        return g_variant_new("(i@a{sv}@av)", parent, props, g_variant_new_array(G_VARIANT_TYPE_VARIANT, nullptr, 0));
    }
    GVariantBuilder children;
    g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
    for (const auto& item : items_) {
        GVariant* child = g_variant_new("(i@a{sv}@av)", item.id, item_properties(item),
                                        g_variant_new_array(G_VARIANT_TYPE_VARIANT, nullptr, 0));
        g_variant_builder_add(&children, "v", child);
    }
    GVariantBuilder root;
    g_variant_builder_init(&root, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&root, "{sv}", "children-display", g_variant_new_string("submenu"));
    return g_variant_new("(i@a{sv}@av)", 0, g_variant_builder_end(&root), g_variant_builder_end(&children));
}

const Indicator::Item* Indicator::find(int id) const {
    for (const auto& item : items_)
        if (item.id == id) return &item;
    return nullptr;
}

void Indicator::run(int id) {
    const Item* item = find(id);
    if (!item || !item->action || !item->enabled) return;
    auto* action = new std::function<void()>(item->action);
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
                    +[](gpointer data) -> gboolean {
                        (*static_cast<std::function<void()>*>(data))();
                        return G_SOURCE_REMOVE;
                    },
                    action, +[](gpointer data) { delete static_cast<std::function<void()>*>(data); });
}

void Indicator::menu_method(const char* method, GVariant* params, GDBusMethodInvocation* inv) {
    std::string m = method;
    if (m == "GetLayout") {
        gint32 parent = 0, depth = 0;
        GVariant* names = nullptr;
        g_variant_get(params, "(ii@as)", &parent, &depth, &names);
        g_variant_unref(names);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u@(ia{sv}av))", revision_, layout(parent)));
    } else if (m == "GetGroupProperties") {
        GVariant* ids = nullptr;
        GVariant* names = nullptr;
        g_variant_get(params, "(@ai@as)", &ids, &names);
        GVariantBuilder out;
        g_variant_builder_init(&out, G_VARIANT_TYPE("a(ia{sv})"));
        gsize n = 0;
        const gint32* list = static_cast<const gint32*>(g_variant_get_fixed_array(ids, &n, sizeof(gint32)));
        for (const auto& item : items_) {
            bool wanted = n == 0;
            for (gsize i = 0; i < n && !wanted; i++) wanted = list[i] == item.id;
            if (wanted) g_variant_builder_add(&out, "(i@a{sv})", item.id, item_properties(item));
        }
        g_variant_unref(ids);
        g_variant_unref(names);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(@a(ia{sv}))", g_variant_builder_end(&out)));
    } else if (m == "GetProperty") {
        gint32 id = 0;
        const gchar* name = nullptr;
        g_variant_get(params, "(i&s)", &id, &name);
        const Item* item = find(id);
        GVariant* value = nullptr;
        if (item) {
            GVariant* props = g_variant_ref_sink(item_properties(*item));
            value = g_variant_lookup_value(props, name, nullptr);
            g_variant_unref(props);
        }
        if (!value) value = g_variant_new_string("");
        g_dbus_method_invocation_return_value(inv, g_variant_new("(v)", value));
    } else if (m == "Event") {
        gint32 id = 0;
        const gchar* event = nullptr;
        GVariant* data = nullptr;
        guint32 ts = 0;
        g_variant_get(params, "(i&svu)", &id, &event, &data, &ts);
        bool clicked = g_strcmp0(event, "clicked") == 0;
        g_variant_unref(data);
        g_dbus_method_invocation_return_value(inv, nullptr);
        if (clicked) run(id);
    } else if (m == "EventGroup") {
        GVariantIter* iter = nullptr;
        g_variant_get(params, "(a(isvu))", &iter);
        gint32 id = 0;
        const gchar* event = nullptr;
        GVariant* data = nullptr;
        guint32 ts = 0;
        std::vector<int> clicked;
        while (g_variant_iter_loop(iter, "(i&svu)", &id, &event, &data, &ts))
            if (g_strcmp0(event, "clicked") == 0) clicked.push_back(id);
        g_variant_iter_free(iter);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(@ai)", g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0)));
        for (int c : clicked) run(c);
    } else if (m == "AboutToShow") {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", FALSE));
    } else if (m == "AboutToShowGroup") {
        g_dbus_method_invocation_return_value(
            inv, g_variant_new("(@ai@ai)", g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0),
                               g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0)));
    } else {
        g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method);
    }
}

std::string Indicator::status_text() const {
    // Passive hides this item while the Nord Buds GNOME extension shows its own.
    return Settings::get().extension_active() ? "Passive" : "Active";
}
