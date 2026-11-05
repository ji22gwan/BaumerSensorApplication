// OxUdpSimulator.h - sends OX200 UDP profile packets (layout from the OX200 operating manual,
// firmware 1.7, chapter 5.6 "UDP"), so the UDP receive path can be tested without a sensor.
//
// Datagram  = 8 byte header + body fragment
//   header : BlockId u32 | FrameType u8 (0 single, 1 first, 2 consecutive) | reserved u8 | FrameCount/Index u16
//   body   : MessageType u8 (1 Z, 2 I, 3 Z-I) | ConfigMode | NTP | ValuesValid | Alarm | Quality u8 |
//            rate f32 @6 | tsSec u32 @10 | tsUs u32 @14 | encoder u16 @18 | length u32 @20 | points @24
//   point  : x i16, then z u16 (Z), i u16 (I) or z u16 + i u16 (Z-I)     (16 bit variant)
// All values little endian.
#pragma once

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace baumer::sim {

class OxUdpSimulator {
public:
    OxUdpSimulator(const std::string& ip, std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) throw std::runtime_error("socket() failed");
        dst_.sin_family = AF_INET;
        dst_.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &dst_.sin_addr) != 1) throw std::runtime_error("bad ip " + ip);
    }
    ~OxUdpSimulator() {
        if (fd_ >= 0) ::close(fd_);
    }
    OxUdpSimulator(const OxUdpSimulator&) = delete;
    OxUdpSimulator& operator=(const OxUdpSimulator&) = delete;

    /// type: 1 = Z profile, 2 = I profile, 3 = Z-I profile (16 bit). Returns the number of datagrams sent.
    int sendProfile(std::uint8_t type, const std::vector<std::int16_t>& x, const std::vector<std::uint16_t>& z,
                    const std::vector<std::uint16_t>& intensity, float rateHz, std::uint32_t tsSec,
                    std::uint32_t tsUs, std::uint16_t encoder) {
        const std::uint32_t n = static_cast<std::uint32_t>(x.size());
        std::vector<std::uint8_t> body(24);
        body[0] = type;
        body[1] = 0;  // ConfigMode
        body[2] = 1;  // NTP synced
        body[3] = 1;  // values valid
        body[4] = 0;  // alarm
        body[5] = 0;  // quality
        put(body, 6, rateHz);
        put(body, 10, tsSec);
        put(body, 14, tsUs);
        put(body, 18, encoder);
        put(body, 20, n);
        for (std::uint32_t i = 0; i < n; ++i) {
            put16(body, static_cast<std::uint16_t>(x[i]));
            if (type == 1 || type == 3) put16(body, z[i]);
            if (type == 2 || type == 3) put16(body, intensity[i]);
        }

        const std::size_t chunk = kMaxDatagram - 8;
        const std::size_t parts = (body.size() + chunk - 1) / chunk;
        int sent = 0;
        for (std::size_t p = 0; p < parts; ++p) {
            std::vector<std::uint8_t> d(8);
            put(d, 0, blockId_);
            d[4] = parts == 1 ? 0 : (p == 0 ? 1 : 2);
            d[5] = 0;
            put(d, 6, static_cast<std::uint16_t>(parts == 1 ? 0 : (p == 0 ? parts : p)));
            const std::size_t off = p * chunk;
            const std::size_t len = std::min(chunk, body.size() - off);
            d.insert(d.end(), body.begin() + static_cast<std::ptrdiff_t>(off),
                     body.begin() + static_cast<std::ptrdiff_t>(off + len));
            if (::sendto(fd_, d.data(), d.size(), 0, reinterpret_cast<const sockaddr*>(&dst_), sizeof dst_) < 0)
                throw std::runtime_error("sendto failed");
            ++sent;
        }
        ++blockId_;
        return sent;
    }

private:
    static constexpr std::size_t kMaxDatagram = 1400;

    template <typename T>
    static void put(std::vector<std::uint8_t>& b, std::size_t off, T v) {
        std::memcpy(&b[off], &v, sizeof v);  // host is little endian (x86 / arm)
    }
    static void put16(std::vector<std::uint8_t>& b, std::uint16_t v) {
        b.push_back(static_cast<std::uint8_t>(v & 0xFF));
        b.push_back(static_cast<std::uint8_t>(v >> 8));
    }

    int fd_ = -1;
    sockaddr_in dst_{};
    std::uint32_t blockId_ = 0;
};

}  // namespace baumer::sim
