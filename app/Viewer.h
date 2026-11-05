// Viewer.h - Dear ImGui / ImPlot viewer for a SensorSource
#pragma once

#include <BaumerSensor/SensorSource.h>

#include <memory>
#include <string>

namespace baumer {

struct ViewerOptions {
    SensorConfig cfg;
    bool demo = false;
    bool autostart = false;  ///< connect + start polling + start image right after launch
    bool startUdp = false;   ///< start receiving the UDP profile stream right after launch (no connection)
    float uiScale = 0.f;     ///< 0 = take it from the monitor
    int exitAfterFrames = 0; ///< >0: exit after N frames (automated screenshot test)
    std::string screenshotPath;  ///< if set (with exitAfterFrames): write a PPM of the last frame
};

class Viewer {
public:
    Viewer(std::unique_ptr<SensorSource> source, ViewerOptions opt);
    ~Viewer();
    int run();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace baumer
