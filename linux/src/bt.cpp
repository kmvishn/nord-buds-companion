#include "bt.h"

#include <endian.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

// Kernel Bluetooth socket ABI, declared here so libbluetooth-dev is not needed.
namespace {

constexpr int kAfBluetooth = 31;
constexpr int kBtProtoL2cap = 0;
constexpr int kBtProtoRfcomm = 3;
constexpr uint16_t kSdpPsm = 1;

struct __attribute__((packed)) BdAddr {
    uint8_t b[6];
};

struct SockaddrL2 {
    sa_family_t family;
    uint16_t psm;
    BdAddr bdaddr;
    uint16_t cid;
    uint8_t bdaddr_type;
};

struct SockaddrRc {
    sa_family_t family;
    BdAddr bdaddr;
    uint8_t channel;
};

bool parse_address(const std::string& s, BdAddr& out) {
    unsigned v[6];
    if (std::sscanf(s.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return false;
    for (int i = 0; i < 6; i++) out.b[i] = static_cast<uint8_t>(v[5 - i]);
    return true;
}

std::string errno_text(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

bool connect_with_timeout(int fd, const sockaddr* sa, socklen_t len, int timeout_ms, std::string* err) {
    if (connect(fd, sa, len) == 0) return true;
    if (errno != EINPROGRESS && errno != EAGAIN) {
        *err = errno_text("connect");
        return false;
    }
    pollfd p{fd, POLLOUT, 0};
    int r = poll(&p, 1, timeout_ms);
    if (r <= 0) {
        *err = r == 0 ? "connect: timed out" : errno_text("poll");
        return false;
    }
    int so_error = 0;
    socklen_t so_len = sizeof so_error;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len);
    if (so_error != 0) {
        errno = so_error;
        *err = errno_text("connect");
        return false;
    }
    return true;
}

bool receive_with_timeout(int fd, std::vector<uint8_t>& out, int timeout_ms, std::string* err) {
    pollfd p{fd, POLLIN, 0};
    int r = poll(&p, 1, timeout_ms);
    if (r <= 0) {
        *err = r == 0 ? "SDP: no answer" : errno_text("poll");
        return false;
    }
    out.resize(4096);
    ssize_t n = recv(fd, out.data(), out.size(), 0);
    if (n <= 0) {
        *err = n == 0 ? "SDP: connection closed" : errno_text("recv");
        return false;
    }
    out.resize(static_cast<size_t>(n));
    return true;
}

}  // namespace

namespace bt {

int sdp_rfcomm_channel(const std::string& address, const uint8_t uuid[16], std::string* err) {
    BdAddr addr;
    if (!parse_address(address, addr)) {
        *err = "invalid Bluetooth address " + address;
        return -1;
    }
    int fd = socket(kAfBluetooth, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, kBtProtoL2cap);
    if (fd < 0) {
        *err = errno_text("socket");
        return -1;
    }
    SockaddrL2 sa{};
    sa.family = kAfBluetooth;
    sa.psm = htole16(kSdpPsm);
    sa.bdaddr = addr;
    if (!connect_with_timeout(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa, 8000, err)) {
        close(fd);
        return -1;
    }

    // ServiceSearchAttributeRequest: pattern = {uuid}, all attributes.
    std::vector<uint8_t> pattern = {0x35, 0x11, 0x1C};
    pattern.insert(pattern.end(), uuid, uuid + 16);
    const uint8_t attributes[] = {0x35, 0x05, 0x0A, 0x00, 0x00, 0xFF, 0xFF};
    std::vector<uint8_t> continuation = {0x00};
    std::vector<uint8_t> data;
    uint16_t transaction = 1;

    for (int round = 0; round < 16; round++) {
        std::vector<uint8_t> params = pattern;
        params.push_back(0xFF);
        params.push_back(0xFF);
        params.insert(params.end(), attributes, attributes + sizeof attributes);
        params.insert(params.end(), continuation.begin(), continuation.end());

        std::vector<uint8_t> pdu = {0x06, uint8_t(transaction >> 8), uint8_t(transaction),
                                    uint8_t(params.size() >> 8), uint8_t(params.size())};
        pdu.insert(pdu.end(), params.begin(), params.end());
        transaction++;

        if (send(fd, pdu.data(), pdu.size(), 0) < 0) {
            *err = errno_text("SDP send");
            close(fd);
            return -1;
        }
        std::vector<uint8_t> reply;
        if (!receive_with_timeout(fd, reply, 8000, err)) {
            close(fd);
            return -1;
        }
        if (reply.size() < 7 || reply[0] != 0x07) {
            *err = "SDP: error response";
            close(fd);
            return -1;
        }
        size_t n = (size_t(reply[5]) << 8) | reply[6];
        if (reply.size() < 8 + n) {
            *err = "SDP: short response";
            close(fd);
            return -1;
        }
        data.insert(data.end(), reply.begin() + 7, reply.begin() + 7 + long(n));
        continuation.assign(reply.begin() + 7 + long(n), reply.end());
        if (continuation.empty() || continuation[0] == 0) break;
    }
    close(fd);

    // ProtocolDescriptorList entry: UUID16 0x0003 (RFCOMM) followed by uint8 channel.
    for (size_t i = 0; i + 4 < data.size(); i++) {
        if (data[i] == 0x19 && data[i + 1] == 0x00 && data[i + 2] == 0x03 && data[i + 3] == 0x08)
            return data[i + 4];
    }
    *err = "control service not offered";
    return -1;
}

int rfcomm_connect(const std::string& address, int channel, std::string* err) {
    BdAddr addr;
    if (!parse_address(address, addr)) {
        *err = "invalid Bluetooth address " + address;
        return -1;
    }
    int fd = socket(kAfBluetooth, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, kBtProtoRfcomm);
    if (fd < 0) {
        *err = errno_text("socket");
        return -1;
    }
    SockaddrRc sa{};
    sa.family = kAfBluetooth;
    sa.bdaddr = addr;
    sa.channel = static_cast<uint8_t>(channel);
    if (!connect_with_timeout(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa, 10000, err)) {
        close(fd);
        return -1;
    }
    return fd;
}

}  // namespace bt
