// BaumerSensor/Types.h
// Plain data types of the BaumerSensor library. No dependency on the Baumer SDK headers,
// so code that includes this header does not need OXApi on its include path.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace baumer {

/// Connection / streaming parameters. Change them only while nothing is running.
struct SensorConfig {
    std::string host = "192.168.0.250";   ///< sensor IP or host name (OX default: 192.168.0.250)
    std::uint16_t udpPort = 12345;        ///< local UDP port the profile stream is received on
    /// Divisor that converts raw integer profile values to mm (raw / precision).
    /// It is read from the sensor on connect(); this value is used until then (UDP-only use).
    double precision = 100.0;
};

struct SensorInfo {
    std::string type;
    std::string serialNumber;
    std::string vendor;
    std::string aggregateVersion;
    std::string softwareVersion;
    std::string xUnit;                    ///< unit reported by the sensor (normally "mm")
    std::string zUnit;
    std::uint32_t profileMaxLength = 0;   ///< max number of profile points
    std::uint32_t imageWidth = 0;         ///< full sensor image size in pixels
    std::uint32_t imageHeight = 0;
    double precision = 0.0;               ///< 0 = not read from the sensor
};

/// One measured profile. x/z are in mm, intensity is the raw sensor value.
struct ProfileFrame {
    std::uint32_t blockId = 0;            ///< UDP block id (0 for polled profiles)
    std::uint64_t timestamp = 0;          ///< sensor time stamp, raw value as delivered by the SDK
    std::uint32_t quality = 0;
    float measurementRateHz = 0.f;        ///< as reported by the sensor (UDP only)
    std::uint16_t encoder = 0;            ///< encoder value (UDP only)
    bool valuesValid = true;
    bool alarm = false;
    bool zValid = false;
    bool intensityValid = false;
    double hostTime = 0.0;                ///< steady-clock arrival time on this PC [s]
    std::vector<double> x;                ///< [mm]
    std::vector<double> z;                ///< [mm], empty if !zValid
    std::vector<std::uint16_t> intensity; ///< empty if !intensityValid
};

/// Raw 8 bit camera image of the sensor (the ROI that the sensor currently reads out).
struct ImageFrame {
    std::uint32_t width = 0;              ///< RoiWidth
    std::uint32_t height = 0;             ///< RoiHeight
    std::uint32_t columnOffset = 0;
    std::uint32_t rowOffset = 0;
    std::uint32_t columnBinning = 1;
    std::uint32_t rowBinning = 1;
    double hostTime = 0.0;
    std::vector<std::uint8_t> pixels;     ///< width*height, row-major, 8 bit gray
};

struct SensorStats {
    bool controlConnected = false;
    bool profileRunning = false;
    bool imageRunning = false;
    std::string profileSource;            ///< "", "poll" or "udp"
    std::uint64_t profilesReceived = 0;
    std::uint64_t imagesReceived = 0;
    std::uint64_t packetErrors = 0;       ///< UDP lost / out-of-sequence / invalid packets
    double profileFps = 0.0;              ///< measured on this PC
    double imageFps = 0.0;
    double precision = 0.0;               ///< precision currently used for x/z conversion
};

/// A UDP stream the sensor can send (from GetUdpStreamingInfo).
struct UdpStreamOption {
    std::uint32_t id = 0;
    std::string name;
    bool active = false;
};

}  // namespace baumer
