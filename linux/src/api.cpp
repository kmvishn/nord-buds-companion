#include "api.h"

#include "buds.h"
#include "model.h"
#include "settings.h"

namespace {

constexpr const char* kPath = "/com/nordbuds/Companion/Buds";
constexpr const char* kInterface = "com.nordbuds.Companion.Buds";
constexpr const char* kPanelName = "com.nordbuds.Companion.Panel";

constexpr const char* kXml = R"XML(
<node>
  <interface name="com.nordbuds.Companion.Buds">
    <method name="GetState"><arg type="a{sv}" name="state" direction="out"/></method>
    <method name="SetEq"><arg type="i" name="preset" direction="in"/></method>
    <method name="SetGameMode"><arg type="b" name="on" direction="in"/></method>
    <method name="Find"><arg type="b" name="on" direction="in"/></method>
    <method name="Connect"/>
    <method name="Disconnect"/>
    <method name="OpenWindow"/>
    <signal name="StateChanged"><arg type="a{sv}" name="state"/></signal>
  </interface>
</node>
)XML";

const char* link_key(LinkState l) {
    switch (l) {
    case LinkState::NoDevice: return "no-device";
    case LinkState::Disconnected: return "disconnected";
    case LinkState::Connecting: return "connecting";
    case LinkState::Connected: return "connected";
    }
    return "";
}

void method_cb(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method, GVariant* params,
               GDBusMethodInvocation* inv, gpointer self) {
    static_cast<ServiceApi*>(self)->call(method, params, inv);
}

const GDBusInterfaceVTable kVtable = {method_cb, nullptr, nullptr, {}};

}  // namespace

ServiceApi::ServiceApi(GDBusConnection* bus, Buds& buds, std::function<void()> open_window)
    : bus_(G_DBUS_CONNECTION(g_object_ref(bus))), buds_(buds), open_window_(std::move(open_window)) {
    GError* err = nullptr;
    node_ = g_dbus_node_info_new_for_xml(kXml, &err);
    if (node_) reg_ = g_dbus_connection_register_object(bus_, kPath, node_->interfaces[0], &kVtable, this, nullptr, &err);
    if (err) {
        g_warning("api: %s", err->message);
        g_error_free(err);
    }
    panel_watch_ = g_bus_watch_name_on_connection(
        bus_, kPanelName, G_BUS_NAME_WATCHER_FLAGS_NONE,
        +[](GDBusConnection*, const gchar*, const gchar*, gpointer) { Settings::get().set_extension_active(true); },
        +[](GDBusConnection*, const gchar*, gpointer) { Settings::get().set_extension_active(false); }, nullptr,
        nullptr);
}

ServiceApi::~ServiceApi() {
    if (panel_watch_) g_bus_unwatch_name(panel_watch_);
    if (reg_) g_dbus_connection_unregister_object(bus_, reg_);
    if (node_) g_dbus_node_info_unref(node_);
    g_object_unref(bus_);
}

GVariant* ServiceApi::state() const {
    const auto& st = buds_.state();
    const auto& panel = Settings::get().panel();
    bool ready = buds_.ready();
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    auto add = [&b](const char* key, GVariant* v) { g_variant_builder_add(&b, "{sv}", key, v); };
    add("link", g_variant_new_string(link_key(st.link)));
    add("ready", g_variant_new_boolean(ready));
    add("bt-connected", g_variant_new_boolean(st.bt_connected));
    add("name", g_variant_new_string(st.name.c_str()));
    add("left", g_variant_new_int32(ready ? st.battery.left.level : -1));
    add("right", g_variant_new_int32(ready ? st.battery.right.level : -1));
    add("case", g_variant_new_int32(ready ? st.battery.box.level : -1));
    add("left-charging", g_variant_new_boolean(ready && st.battery.left.charging));
    add("right-charging", g_variant_new_boolean(ready && st.battery.right.charging));
    add("case-charging", g_variant_new_boolean(ready && st.battery.box.charging));
    add("eq", g_variant_new_int32(st.eq));
    add("game-mode", g_variant_new_int32(st.game_mode));
    add("finding", g_variant_new_boolean(st.finding));
    add("can-eq", g_variant_new_boolean(ready && buds_.supports(proto::kEqSet)));
    add("can-game-mode", g_variant_new_boolean(ready && buds_.supports(proto::kFeatureSet)));
    add("can-find", g_variant_new_boolean(ready && buds_.supports(proto::kFind)));
    GVariantBuilder presets;
    g_variant_builder_init(&presets, G_VARIANT_TYPE("a(is)"));
    for (const auto& p : model::eq_presets()) g_variant_builder_add(&presets, "(is)", int(p.id), p.name);
    add("eq-presets", g_variant_builder_end(&presets));
    add("panel-position", g_variant_new_string(panel.position.c_str()));
    add("panel-order", g_variant_new_int32(panel.order));
    add("panel-label", g_variant_new_string(panel.label.c_str()));
    add("label", g_variant_new_string(panel_label(st, ready, panel.label).c_str()));
    return g_variant_builder_end(&b);
}

void ServiceApi::emit_changed() {
    if (!reg_) return;
    g_dbus_connection_emit_signal(bus_, nullptr, kPath, kInterface, "StateChanged",
                                  g_variant_new("(@a{sv})", state()), nullptr);
}

void ServiceApi::call(const char* method, GVariant* params, GDBusMethodInvocation* inv) {
    std::string m = method;
    if (m == "GetState") {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(@a{sv})", state()));
        return;
    }
    bool ok = true;
    if (m == "SetEq") {
        gint32 id = 0;
        g_variant_get(params, "(i)", &id);
        ok = buds_.set_eq(id);
    } else if (m == "SetGameMode") {
        gboolean on = FALSE;
        g_variant_get(params, "(b)", &on);
        ok = buds_.set_game_mode(on);
    } else if (m == "Find") {
        gboolean on = FALSE;
        g_variant_get(params, "(b)", &on);
        ok = buds_.set_find(on);
    } else if (m == "Connect") {
        buds_.bluetooth_connect();
    } else if (m == "Disconnect") {
        buds_.bluetooth_disconnect();
    } else if (m == "OpenWindow") {
        open_window_();
    }
    if (ok)
        g_dbus_method_invocation_return_value(inv, nullptr);
    else
        g_dbus_method_invocation_return_dbus_error(inv, "com.nordbuds.Companion.Error.NotReady",
                                                   "The earbuds are not ready for that");
}
