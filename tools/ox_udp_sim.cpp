// ox_udp_sim - fake OX200 UDP profile stream (for testing the viewer's "UDP stream" mode without a sensor)
//   ox_udp_sim [--ip 127.0.0.1] [--port 12345] [--hz 50] [--points 640] [--type 3]
// Raw values use precision 100 (raw / 100 = mm): the viewer default.
#include "OxUdpSimulator.h"

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
volatile std::sig_atomic_t gStop = 0;
void onSignal(int) { gStop = 1; }
}  // namespace

int main(int argc, char** argv) {
    std::string ip = "127.0.0.1";
    int port = 12345, points = 640, type = 3;
    double hz = 50.0;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--ip") ip = argv[i + 1];
        else if (a == "--port") port = std::atoi(argv[i + 1]);
        else if (a == "--hz") hz = std::atof(argv[i + 1]);
        else if (a == "--points") points = std::atoi(argv[i + 1]);
        else if (a == "--type") type = std::atoi(argv[i + 1]);
        else {
            std::cerr << "usage: ox_udp_sim [--ip A] [--port N] [--hz F] [--points N] [--type 1|2|3]\n";
            return 2;
        }
    }
    std::signal(SIGINT, onSignal);
    baumer::sim::OxUdpSimulator sim(ip, static_cast<std::uint16_t>(port));
    std::cout << "sending type " << type << ", " << points << " points @ " << hz << " Hz to " << ip << ":" << port
              << " (Ctrl+C to stop)\n";

    const auto t0 = std::chrono::steady_clock::now();
    const auto period = std::chrono::duration<double>(1.0 / hz);
    auto next = t0;
    std::vector<std::int16_t> x(points);
    std::vector<std::uint16_t> z(points), in(points);
    while (!gStop) {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        for (int i = 0; i < points; ++i) {
            const double xm = -14.0 + 28.0 * i / (points - 1);          // mm
            double zm = 205.0 - (xm > 0 ? 12.0 : 0.0);                  // step
            zm -= 6.0 * std::exp(-std::pow((xm - 7.0 * std::sin(t * 0.6)) / 1.8, 2.0));  // moving bump
            x[i] = static_cast<std::int16_t>(std::lround(xm * 100.0));
            z[i] = static_cast<std::uint16_t>(std::lround(zm * 100.0));
            in[i] = static_cast<std::uint16_t>(2000 + 200 * std::sin(0.05 * i + t));
        }
        const auto us = static_cast<std::uint64_t>(t * 1e6);
        sim.sendProfile(static_cast<std::uint8_t>(type), x, z, in, static_cast<float>(hz),
                        static_cast<std::uint32_t>(us / 1000000), static_cast<std::uint32_t>(us % 1000000), 0);
        next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
        std::this_thread::sleep_until(next);
    }
    return 0;
}
