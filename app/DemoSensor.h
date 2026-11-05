// DemoSensor.h - synthetic SensorSource for trying the viewer without a sensor (--demo)
#pragma once

#include <BaumerSensor/SensorSource.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace baumer {

class DemoSensor final : public SensorSource {
public:
    DemoSensor();
    ~DemoSensor() override;

    void setConfig(const SensorConfig& cfg) override { cfg_ = cfg; }
    bool connect(std::string& err) override;
    void disconnect() override;
    bool isConnected() const override { return connected_; }
    SensorInfo info() const override;

    bool startPolling(double hz, std::string& err) override;
    bool startUdp(std::string& err) override;
    void stopProfile() override;
    bool startImage(double hz, std::string& err) override;
    void stopImage() override;

    bool getUdpStreamOptions(std::vector<UdpStreamOption>& out, std::string& err) override;
    bool applyUdpStreams(const std::string& localIp, const std::vector<std::uint32_t>& ids,
                         std::string& err) override;

    bool latestProfile(ProfileFrame& out, std::uint64_t& seq) override;
    bool latestImage(ImageFrame& out, std::uint64_t& seq) override;
    SensorStats stats() const override;
    std::vector<std::string> drainLog() override;

private:
    void profileLoop(double hz);
    void imageLoop(double hz);
    void log(const std::string& s);

    SensorConfig cfg_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> profRun_{false}, imgRun_{false};
    std::thread profThread_, imgThread_;
    std::string source_;

    std::mutex mu_;
    ProfileFrame profile_;
    std::uint64_t profileSeq_ = 0;
    ImageFrame image_;
    std::uint64_t imageSeq_ = 0;
    std::vector<std::string> logLines_;
    std::atomic<std::uint64_t> nProfiles_{0}, nImages_{0};
    std::atomic<double> profFps_{0}, imgFps_{0};
};

}  // namespace baumer
