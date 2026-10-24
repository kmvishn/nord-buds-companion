#pragma once

#include <gio/gio.h>

#include <functional>

class Buds;

// Session-bus API used by the GNOME Shell extension:
// com.nordbuds.Companion  /com/nordbuds/Companion/Buds  com.nordbuds.Companion.Buds
class ServiceApi {
public:
    ServiceApi(GDBusConnection* bus, Buds& buds, std::function<void()> open_window);
    ~ServiceApi();
    ServiceApi(const ServiceApi&) = delete;
    ServiceApi& operator=(const ServiceApi&) = delete;

    void emit_changed();

    GVariant* state() const;
    void call(const char* method, GVariant* params, GDBusMethodInvocation* inv);

private:
    GDBusConnection* bus_;
    Buds& buds_;
    std::function<void()> open_window_;
    GDBusNodeInfo* node_ = nullptr;
    guint reg_ = 0;
    guint panel_watch_ = 0;
};
