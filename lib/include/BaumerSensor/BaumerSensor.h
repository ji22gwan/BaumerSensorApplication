// BaumerSensor/BaumerSensor.h
// Thin, thread-safe wrapper around the Baumer OX-Line C++ SDK (libOXApi.so) for OXP200 profile
// sensors. Intended to be linked as a library into other controllers.
//
// Minimal use (UDP stream, no control connection):
//     baumer::BaumerSensor s;
//     s.setConfig({.host = "192.168.0.250", .udpPort = 12345, .precision = 100.0});
//     std::string err;
//     s.startUdp(err);
//     baumer::ProfileFrame f; std::uint64_t seq = 0;
//     while (running) { if (s.latestProfile(f, seq)) use(f); }
//
// Minimal use (polling over the control connection, also reads precision from the sensor):
//     s.connect(err); s.startPolling(10.0, err); s.startImage(2.0, err);
#pragma once

#include <BaumerSensor/SensorSource.h>

#include <memory>

namespace baumer {

class BaumerSensor final : public SensorSource {
public:
    BaumerSensor();
    ~BaumerSensor() override;
    BaumerSensor(const BaumerSensor&) = delete;
    BaumerSensor& operator=(const BaumerSensor&) = delete;

    void setConfig(const SensorConfig& cfg) override;

    bool connect(std::string& err) override;
    void disconnect() override;
    bool isConnected() const override;
    SensorInfo info() const override;

    bool startPolling(double hz, std::string& err) override;
    bool startUdp(std::string& err) override;
    void stopProfile() override;

    bool startImage(double hz, std::string& err) override;
    void stopImage() override;

    bool getUdpStreamOptions(std::vector<UdpStreamOption>& out, std::string& err) override;
    bool applyUdpStreams(const std::string& localIp, const std::vector<std::uint32_t>& streamIds,
                         std::string& err) override;

    bool latestProfile(ProfileFrame& out, std::uint64_t& seq) override;
    bool latestImage(ImageFrame& out, std::uint64_t& seq) override;

    SensorStats stats() const override;
    std::vector<std::string> drainLog() override;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

/// Returns the IPv4 address of the local interface that is in the same subnet as `sensorHost`
/// (empty if none found). Used to pre-fill the UDP target address.
std::string guessLocalIp(const std::string& sensorHost);

}  // namespace baumer
