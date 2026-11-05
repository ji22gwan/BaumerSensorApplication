// udp_loopback_test - end-to-end test of the UDP receive path without a sensor:
//   OxUdpSimulator --UDP/127.0.0.1--> libOXApi.so (OxStream) --> BaumerSensor --> ProfileFrame
// The packet layout comes from the manual, the parser is Baumer's own library, so this also checks that
// our understanding of the packet format matches the SDK.
#include "../tools/OxUdpSimulator.h"

#include <BaumerSensor/BaumerSensor.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;

static int g_fail = 0;
#define CHECK(cond, msg)                                                         \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "FAIL: " << msg << "  [" #cond "]  line " << __LINE__ << "\n"; \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

// sends `count` profiles of the given type and waits until the receiver delivered a frame of that kind
static bool runCase(baumer::BaumerSensor& s, baumer::sim::OxUdpSimulator& sim, std::uint8_t type, int points,
                    baumer::ProfileFrame& out, std::uint64_t& seq) {
    std::vector<std::int16_t> x(points);
    std::vector<std::uint16_t> z(points), in(points);
    for (int i = 0; i < points; ++i) {
        x[i] = static_cast<std::int16_t>((i - points / 2) * 5);  // -> (i-320)*0.05 mm at precision 100
        z[i] = static_cast<std::uint16_t>(15000 + i);            // -> 150.00 + 0.01*i mm
        in[i] = static_cast<std::uint16_t>(1000 + i);
    }
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (std::chrono::steady_clock::now() < deadline) {
        sim.sendProfile(type, x, z, in, 200.f, 12, 345678, 7);
        std::this_thread::sleep_for(5ms);
        std::uint64_t before = seq;
        if (s.latestProfile(out, seq) && seq != before) {
            // wait for a frame of the wanted type (an older one may still be queued)
            const bool wantZ = (type == 1 || type == 3), wantI = (type == 2 || type == 3);
            if (out.zValid == wantZ && out.intensityValid == wantI) return true;
        }
    }
    return false;
}

int main() {
    const std::uint16_t port = static_cast<std::uint16_t>(30000 + (::getpid() % 20000));
    baumer::BaumerSensor s;
    baumer::SensorConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.udpPort = port;
    cfg.precision = 100.0;
    s.setConfig(cfg);

    std::string err;
    if (!s.startUdp(err)) {
        std::cerr << "startUdp failed: " << err << "\n";
        return 1;
    }
    baumer::sim::OxUdpSimulator sim("127.0.0.1", port);
    baumer::ProfileFrame f;
    std::uint64_t seq = 0;
    const int N = 640;  // 640 points * 6 byte > one datagram -> exercises fragment reassembly

    // ---- Z-I profile, fragmented
    CHECK(runCase(s, sim, 3, N, f, seq), "no Z-I profile received");
    CHECK(f.x.size() == static_cast<std::size_t>(N), "Z-I length " << f.x.size());
    if (f.x.size() == static_cast<std::size_t>(N) && f.z.size() == static_cast<std::size_t>(N) &&
        f.intensity.size() == static_cast<std::size_t>(N)) {
        CHECK(std::fabs(f.x[0] - (-16.0)) < 1e-9, "x[0]=" << f.x[0]);
        CHECK(std::fabs(f.x[N - 1] - 15.95) < 1e-9, "x[last]=" << f.x[N - 1]);
        CHECK(std::fabs(f.z[0] - 150.0) < 1e-9, "z[0]=" << f.z[0]);
        CHECK(std::fabs(f.z[N - 1] - 156.39) < 1e-9, "z[last]=" << f.z[N - 1]);
        CHECK(f.intensity[10] == 1010, "I[10]=" << f.intensity[10]);
        CHECK(f.encoder == 7, "encoder=" << f.encoder);
        CHECK(std::fabs(f.measurementRateHz - 200.f) < 1e-3, "rate=" << f.measurementRateHz);
        CHECK(f.valuesValid && !f.alarm, "flags");
    }

    // ---- Z profile only
    CHECK(runCase(s, sim, 1, N, f, seq), "no Z profile received");
    CHECK(f.zValid && !f.intensityValid && f.intensity.empty(), "Z-only flags");
    CHECK(f.z.size() == static_cast<std::size_t>(N), "Z-only length");

    // ---- I profile only
    CHECK(runCase(s, sim, 2, N, f, seq), "no I profile received");
    CHECK(!f.zValid && f.intensityValid && f.z.empty(), "I-only flags");
    CHECK(f.intensity.size() == static_cast<std::size_t>(N) && f.intensity[N - 1] == 1000 + N - 1, "I-only data");

    // ---- small single-datagram profile
    CHECK(runCase(s, sim, 3, 50, f, seq), "no small profile received");
    CHECK(f.x.size() == 50, "small length " << f.x.size());

    const auto st = s.stats();
    s.stopProfile();
    std::cout << "profiles received: " << st.profilesReceived << ", udp errors: " << st.packetErrors << "\n";
    for (const auto& l : s.drainLog()) std::cout << "  log: " << l << "\n";

    if (g_fail) {
        std::cerr << g_fail << " check(s) failed\n";
        return 1;
    }
    std::cout << "udp_loopback_test: OK\n";
    return 0;
}
