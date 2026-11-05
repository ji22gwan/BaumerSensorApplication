// BaumerSensor/SensorSource.h
// Abstract data source. The viewer only talks to this interface, so it can run on the real
// sensor (BaumerSensor) or on a synthetic source (--demo) without a sensor.
#pragma once

#include <BaumerSensor/Types.h>

#include <cstdint>
#include <string>
#include <vector>

namespace baumer {

class SensorSource {
public:
    virtual ~SensorSource() = default;

    /// Must be called while disconnected / not streaming.
    virtual void setConfig(const SensorConfig& cfg) = 0;

    // ---- control connection (websocket). Needed for image, polled profiles and sensor setup.
    virtual bool connect(std::string& err) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;
    virtual SensorInfo info() const = 0;

    // ---- profile acquisition: either polled over the control connection or UDP stream.
    virtual bool startPolling(double hz, std::string& err) = 0;
    virtual bool startUdp(std::string& err) = 0;
    virtual void stopProfile() = 0;

    // ---- camera image (needs the control connection)
    virtual bool startImage(double hz, std::string& err) = 0;
    virtual void stopImage() = 0;

    // ---- sensor-side UDP setup (needs the control connection, writes sensor settings!)
    virtual bool getUdpStreamOptions(std::vector<UdpStreamOption>& out, std::string& err) = 0;
    virtual bool applyUdpStreams(const std::string& localIp, const std::vector<std::uint32_t>& streamIds,
                                 std::string& err) = 0;

    // ---- results. Copies the newest frame into `out` and returns true only if it is newer than
    //      `seq`; `seq` is updated. Cheap to call every GUI frame.
    virtual bool latestProfile(ProfileFrame& out, std::uint64_t& seq) = 0;
    virtual bool latestImage(ImageFrame& out, std::uint64_t& seq) = 0;

    virtual SensorStats stats() const = 0;
    /// Returns and clears messages produced by the worker threads.
    virtual std::vector<std::string> drainLog() = 0;
};

}  // namespace baumer
