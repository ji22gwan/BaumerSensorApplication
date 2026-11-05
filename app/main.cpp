// BaumerSensorApplication - standalone viewer / test tool for the Baumer OXP200 profile sensor
#include "DemoSensor.h"
#include "Viewer.h"

#include <BaumerSensor/BaumerSensor.h>

#include <cstdlib>
#include <cstring>
#include <iostream>

static void usage(const char* argv0) {
    std::cout << "Usage: " << argv0 << " [options]\n"
              << "  --host <ip>        sensor address            (default 192.168.0.250)\n"
              << "  --port <n>         local UDP port            (default 12345)\n"
              << "  --precision <n>    raw->mm divisor for UDP-only use (default 100, read from sensor on connect)\n"
              << "  --autostart        connect, poll profiles and start camera image immediately\n"
              << "  --udp              only listen for the UDP profile stream (no connection to the sensor)\n"
              << "  --demo             synthetic data, no sensor needed\n"
              << "  --scale <f>        UI scale (default: monitor scale)\n"
              << "  --frames <n> --screenshot <file.ppm>   run n frames, save the last one, exit (testing)\n"
              << "  -h, --help\n";
}

int main(int argc, char** argv) {
    baumer::ViewerOptions opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--host") opt.cfg.host = next("--host");
        else if (a == "--port") opt.cfg.udpPort = static_cast<std::uint16_t>(std::atoi(next("--port")));
        else if (a == "--precision") opt.cfg.precision = std::atof(next("--precision"));
        else if (a == "--autostart") opt.autostart = true;
        else if (a == "--udp") opt.startUdp = true;
        else if (a == "--demo") opt.demo = true;
        else if (a == "--scale") opt.uiScale = static_cast<float>(std::atof(next("--scale")));
        else if (a == "--frames") opt.exitAfterFrames = std::atoi(next("--frames"));
        else if (a == "--screenshot") opt.screenshotPath = next("--screenshot");
        else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown option " << a << "\n";
            usage(argv[0]);
            return 2;
        }
    }

    std::unique_ptr<baumer::SensorSource> src;
    if (opt.demo) src = std::make_unique<baumer::DemoSensor>();
    else src = std::make_unique<baumer::BaumerSensor>();

    baumer::Viewer viewer(std::move(src), opt);
    return viewer.run();
}
