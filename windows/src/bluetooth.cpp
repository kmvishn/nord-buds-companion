#include "bluetooth.h"

#include <ws2bth.h>
#include <windows.h>
#include <bluetoothapis.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <devicetopology.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cstdio>
#include <cwctype>
#include <set>

#include "util.h"

namespace bt {

namespace {

// Bluetooth audio driver property set (bthhfpddi.h): one-shot reconnect / disconnect of the remote device.
const GUID kBtAudioProperties = {0x7fa06c40, 0xb8f6, 0x4c7e, {0x85, 0x56, 0xe8, 0xc3, 0x3a, 0x12, 0xe5, 0x4d}};
constexpr ULONG kOneShotReconnect = 0;
constexpr ULONG kOneShotDisconnect = 1;
constexpr ULONG kPropertyGet = 1;  // KSPROPERTY_TYPE_GET

// KSPROPERTY and IKsControl (ks.h), declared here because SDK and MinGW headers disagree on them.
struct alignas(8) KsProperty {
    GUID set;
    ULONG id;
    ULONG flags;
};
const IID kIKsControl = {0x28f54685, 0x06fd, 0x11d2, {0xb2, 0x7a, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96}};
struct IKsControlLite : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE KsProperty(KsProperty* prop, ULONG prop_len, void* data, ULONG data_len,
                                                 ULONG* returned) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsMethod(void*, ULONG, void*, ULONG, ULONG*) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsEvent(void*, ULONG, void*, ULONG, ULONG*) = 0;
};

template <class T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() {
        if (p) p->Release();
    }
    T* operator->() const { return p; }
    void** out() {
        if (p) p->Release();
        p = nullptr;
        return reinterpret_cast<void**>(&p);
    }
    T** put() { return reinterpret_cast<T**>(out()); }
};

std::string format_address(uint64_t a) {
    char buf[20];
    std::snprintf(buf, sizeof buf, "%02X:%02X:%02X:%02X:%02X:%02X", unsigned(a >> 40) & 0xFF, unsigned(a >> 32) & 0xFF,
                  unsigned(a >> 24) & 0xFF, unsigned(a >> 16) & 0xFF, unsigned(a >> 8) & 0xFF, unsigned(a) & 0xFF);
    return buf;
}

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = wchar_t(std::towlower(c));
    return s;
}

std::wstring prop_string(IPropertyStore* store, const PROPERTYKEY& key) {
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring s;
    if (SUCCEEDED(store->GetValue(key, &v)) && v.vt == VT_LPWSTR && v.pwszVal) s = v.pwszVal;
    PropVariantClear(&v);
    return s;
}

// MinGW's devicetopology.h spells IPart::GetTopologyObject with a trailing "s"; the vtable slot is the same.
HRESULT get_topology_object(IPart* part, IDeviceTopology** out) {
#ifdef __MINGW32__
    return part->GetTopologyObjects(out);
#else
    return part->GetTopologyObject(out);
#endif
}

std::string hresult_text(HRESULT hr) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

}  // namespace

std::vector<Device> paired_devices(bool* radio) {
    std::vector<Device> out;
    BLUETOOTH_FIND_RADIO_PARAMS radio_params = {sizeof radio_params};
    HANDLE radio_handle = nullptr;
    HBLUETOOTH_RADIO_FIND radio_find = BluetoothFindFirstRadio(&radio_params, &radio_handle);
    *radio = radio_find != nullptr;
    if (radio_find) {
        CloseHandle(radio_handle);
        BluetoothFindRadioClose(radio_find);
    }
    if (!*radio) return out;

    BLUETOOTH_DEVICE_SEARCH_PARAMS search = {};
    search.dwSize = sizeof search;
    search.fReturnAuthenticated = TRUE;
    search.fReturnRemembered = TRUE;
    search.fReturnConnected = TRUE;
    search.fReturnUnknown = FALSE;
    search.fIssueInquiry = FALSE;
    BLUETOOTH_DEVICE_INFO info = {};
    info.dwSize = sizeof info;
    HBLUETOOTH_DEVICE_FIND find = BluetoothFindFirstDevice(&search, &info);
    if (!find) return out;
    do {
        if (info.fAuthenticated || info.fRemembered) {
            uint64_t addr = info.Address.ullLong;
            out.push_back({util::utf8(info.szName), format_address(addr), addr, info.fConnected != FALSE});
        }
        info = {};
        info.dwSize = sizeof info;
    } while (BluetoothFindNextDevice(find, &info));
    BluetoothFindDeviceClose(find);
    return out;
}

SOCKET rfcomm_connect(uint64_t addr, const GUID& service, std::string* err) {
    SOCKET s = socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
    if (s == INVALID_SOCKET) {
        *err = "Bluetooth socket: " + util::error_text(DWORD(WSAGetLastError()));
        return INVALID_SOCKET;
    }
    SOCKADDR_BTH sa = {};
    sa.addressFamily = AF_BTH;
    sa.btAddr = addr;
    sa.serviceClassId = service;
    sa.port = 0;  // 0 = look the channel up by serviceClassId
    if (connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == SOCKET_ERROR) {
        int e = WSAGetLastError();
        closesocket(s);
        *err = e == WSASERVICE_NOT_FOUND ? "control service not offered" : util::error_text(DWORD(e));
        return INVALID_SOCKET;
    }
    return s;
}

bool set_audio_connected(const std::string& device_name, bool connect, std::string* err) {
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int matched = 0, done_ok = 0;
    HRESULT last = S_OK;
    {
        Com<IMMDeviceEnumerator> devices;
        Com<IMMDeviceCollection> endpoints;
        UINT count = 0;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                    devices.out())) ||
            FAILED(devices->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED, endpoints.put())) ||
            FAILED(endpoints->GetCount(&count))) {
            *err = "the Windows audio service is not available";
            if (SUCCEEDED(init)) CoUninitialize();
            return false;
        }
        const std::wstring want = lower(util::wide(device_name));
        std::set<std::wstring> filters;
        // Each Bluetooth audio endpoint (stereo, hands-free) hangs off a KS filter owned by the Bluetooth
        // audio driver; asking that filter to reconnect or disconnect acts on the whole device.
        for (UINT i = 0; i < count; i++) {
            Com<IMMDevice> endpoint;
            Com<IPropertyStore> props;
            if (FAILED(endpoints->Item(i, endpoint.put())) || FAILED(endpoint->OpenPropertyStore(STGM_READ, props.put())))
                continue;
            std::wstring names = lower(prop_string(props.p, PKEY_Device_FriendlyName)) + L"\n" +
                                 lower(prop_string(props.p, PKEY_DeviceInterface_FriendlyName));
            if (want.empty() || names.find(want) == std::wstring::npos) continue;

            Com<IDeviceTopology> topology;
            Com<IConnector> connector, other;
            Com<IPart> part;
            Com<IDeviceTopology> filter_topology;
            if (FAILED(endpoint->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, topology.out())) ||
                FAILED(topology->GetConnector(0, connector.put())) || FAILED(connector->GetConnectedTo(other.put())) ||
                FAILED(other->QueryInterface(__uuidof(IPart), part.out())) ||
                FAILED(get_topology_object(part.p, filter_topology.put())))
                continue;
            LPWSTR id = nullptr;
            if (FAILED(filter_topology->GetDeviceId(&id))) continue;
            std::wstring filter_id = id;
            CoTaskMemFree(id);
            if (!filters.insert(filter_id).second) continue;

            Com<IMMDevice> filter;
            Com<IKsControlLite> ks;
            if (FAILED(devices->GetDevice(filter_id.c_str(), filter.put())) ||
                FAILED(filter->Activate(kIKsControl, CLSCTX_ALL, nullptr, ks.out())))
                continue;
            matched++;
            KsProperty prop = {kBtAudioProperties, connect ? kOneShotReconnect : kOneShotDisconnect, kPropertyGet};
            ULONG returned = 0;
            HRESULT hr = ks->KsProperty(&prop, sizeof prop, nullptr, 0, &returned);
            if (SUCCEEDED(hr)) done_ok++;
            else last = hr;
        }
    }
    if (SUCCEEDED(init)) CoUninitialize();
    if (done_ok) return true;
    *err = matched ? "the Bluetooth audio driver refused (" + hresult_text(last) + ")"
                   : "Windows has no audio device for " + device_name + "; use Settings → Bluetooth";
    return false;
}

}  // namespace bt
