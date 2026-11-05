// DemoSensor.cpp
#include "DemoSensor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

namespace baumer {

namespace {

using Clock = std::chrono::steady_clock;
double now() {
    static const auto t0 = Clock::now();
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

constexpr int kPoints = 640;
constexpr double kXmin = -14.0, kXmax = 14.0;  // mm
constexpr double kZnear = 150.0, kZfar = 250.0;  // working range of OXP200-R10C.005 [mm]
constexpr std::uint32_t kImgW = 1280, kImgH = 1024;

// A step at x=0 plus a moving bump. z in mm, 0 = invalid (small gap).
double surfaceZ(double x, double t) {
    double z = 205.0;
    if (x > 0.0) z -= 12.0;
    z -= 6.0 * std::exp(-std::pow((x - 7.0 * std::sin(t * 0.6)) / 1.8, 2.0));
    return z;
}

}  // namespace

DemoSensor::DemoSensor() = default;

DemoSensor::~DemoSensor() {
    disconnect();
    stopProfile();
}

void DemoSensor::log(const std::string& s) {
    std::lock_guard<std::mutex> lk(mu_);
    logLines_.push_back(s);
}

bool DemoSensor::connect(std::string&) {
    connected_ = true;
    log("[demo] connected to synthetic sensor");
    return true;
}

void DemoSensor::disconnect() {
    if (!connected_) return;
    stopImage();
    if (source_ == "poll") stopProfile();
    connected_ = false;
    log("[demo] disconnected");
}

SensorInfo DemoSensor::info() const {
    SensorInfo s;
    s.type = "DEMO (OXP200-R10C.005)";
    s.serialNumber = "0000000";
    s.vendor = "synthetic";
    s.softwareVersion = "demo";
    s.xUnit = s.zUnit = "mm";
    s.profileMaxLength = kPoints;
    s.imageWidth = kImgW;
    s.imageHeight = kImgH;
    s.precision = cfg_.precision;
    return s;
}

bool DemoSensor::startPolling(double hz, std::string&) {
    stopProfile();
    profRun_ = true;
    source_ = "poll";
    profThread_ = std::thread([this, hz] { profileLoop(hz); });
    return true;
}

bool DemoSensor::startUdp(std::string&) {
    stopProfile();
    profRun_ = true;
    source_ = "udp";
    profThread_ = std::thread([this] { profileLoop(100.0); });
    return true;
}

void DemoSensor::stopProfile() {
    profRun_ = false;
    if (profThread_.joinable()) profThread_.join();
    source_.clear();
}

bool DemoSensor::startImage(double hz, std::string& err) {
    if (!connected_) {
        err = "not connected";
        return false;
    }
    stopImage();
    imgRun_ = true;
    imgThread_ = std::thread([this, hz] { imageLoop(hz); });
    return true;
}

void DemoSensor::stopImage() {
    imgRun_ = false;
    if (imgThread_.joinable()) imgThread_.join();
}

bool DemoSensor::getUdpStreamOptions(std::vector<UdpStreamOption>& out, std::string&) {
    out = {{0, "Measurement values", false}, {1, "Z profile", false}, {2, "I profile", false}, {3, "Z-I profile", true}};
    return true;
}

bool DemoSensor::applyUdpStreams(const std::string& ip, const std::vector<std::uint32_t>& ids, std::string&) {
    log("[demo] would stream " + std::to_string(ids.size()) + " stream(s) to " + ip);
    return true;
}

void DemoSensor::profileLoop(double hz) {
    std::mt19937 rng(1);
    std::normal_distribution<double> noise(0.0, 0.02);
    const auto period = std::chrono::duration<double>(1.0 / std::clamp(hz, 1.0, 400.0));
    double winStart = now();
    int cnt = 0;
    while (profRun_) {
        const double t = now();
        ProfileFrame f;
        f.blockId = static_cast<std::uint32_t>(nProfiles_);
        f.timestamp = static_cast<std::uint64_t>(t * 1e6);
        f.measurementRateHz = static_cast<float>(hz);
        f.zValid = f.intensityValid = true;
        f.hostTime = t;
        f.x.resize(kPoints);
        f.z.resize(kPoints);
        f.intensity.resize(kPoints);
        for (int i = 0; i < kPoints; ++i) {
            const double x = kXmin + (kXmax - kXmin) * i / (kPoints - 1);
            f.x[i] = x;
            f.z[i] = surfaceZ(x, t) + noise(rng);
            f.intensity[i] = static_cast<std::uint16_t>(std::fabs(x) < 0.15 ? 90 : 190 + 10 * noise(rng) * 20);
            if (x > 9.0 && x < 10.0) {  // invalid gap, like a shadowed region
                f.z[i] = 0.0;
                f.intensity[i] = 0;
            }
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            profile_ = std::move(f);
            ++profileSeq_;
        }
        ++nProfiles_;
        ++cnt;
        if (t - winStart >= 0.5) {
            profFps_ = cnt / (t - winStart);
            cnt = 0;
            winStart = t;
        }
        std::this_thread::sleep_for(period);
    }
}

void DemoSensor::imageLoop(double hz) {
    std::mt19937 rng(2);
    std::uniform_int_distribution<int> bg(4, 22);
    const auto period = std::chrono::duration<double>(1.0 / std::clamp(hz, 0.1, 30.0));
    double winStart = now();
    int cnt = 0;
    while (imgRun_) {
        const double t = now();
        ImageFrame f;
        f.width = kImgW;
        f.height = kImgH;
        f.hostTime = t;
        f.pixels.resize(static_cast<std::size_t>(kImgW) * kImgH);
        for (auto& p : f.pixels) p = static_cast<std::uint8_t>(bg(rng));
        // laser line: column = x position, row = z position (near = top)
        for (std::uint32_t c = 0; c < kImgW; ++c) {
            const double x = kXmin + (kXmax - kXmin) * c / (kImgW - 1);
            const double z = surfaceZ(x, t);
            const double row = (z - kZnear) / (kZfar - kZnear) * (kImgH - 1);
            if (x > 9.0 && x < 10.0) continue;  // gap
            for (int dr = -8; dr <= 8; ++dr) {
                const int r = static_cast<int>(std::lround(row)) + dr;
                if (r < 0 || r >= static_cast<int>(kImgH)) continue;
                const double v = 255.0 * std::exp(-0.5 * dr * dr / 4.0);
                auto& px = f.pixels[static_cast<std::size_t>(r) * kImgW + c];
                px = static_cast<std::uint8_t>(std::min(255.0, px + v));
            }
        }
        // orientation marker: "L" in the top-left corner (long arm down, short arm right)
        for (int r = 8; r < 88; ++r)
            for (int c = 8; c < 18; ++c) f.pixels[static_cast<std::size_t>(r) * kImgW + c] = 255;
        for (int r = 78; r < 88; ++r)
            for (int c = 8; c < 48; ++c) f.pixels[static_cast<std::size_t>(r) * kImgW + c] = 255;
        {
            std::lock_guard<std::mutex> lk(mu_);
            image_ = std::move(f);
            ++imageSeq_;
        }
        ++nImages_;
        ++cnt;
        if (t - winStart >= 1.0) {
            imgFps_ = cnt / (t - winStart);
            cnt = 0;
            winStart = t;
        }
        std::this_thread::sleep_for(period);
    }
}

bool DemoSensor::latestProfile(ProfileFrame& out, std::uint64_t& seq) {
    std::lock_guard<std::mutex> lk(mu_);
    if (profileSeq_ == seq) return false;
    out = profile_;
    seq = profileSeq_;
    return true;
}

bool DemoSensor::latestImage(ImageFrame& out, std::uint64_t& seq) {
    std::lock_guard<std::mutex> lk(mu_);
    if (imageSeq_ == seq) return false;
    out = image_;
    seq = imageSeq_;
    return true;
}

SensorStats DemoSensor::stats() const {
    SensorStats s;
    s.controlConnected = connected_;
    s.profileRunning = profRun_;
    s.imageRunning = imgRun_;
    s.profileSource = source_;
    s.profilesReceived = nProfiles_;
    s.imagesReceived = nImages_;
    s.profileFps = profRun_ ? profFps_.load() : 0.0;
    s.imageFps = imgRun_ ? imgFps_.load() : 0.0;
    s.precision = cfg_.precision;
    return s;
}

std::vector<std::string> DemoSensor::drainLog() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    out.swap(logLines_);
    return out;
}

}  // namespace baumer
