// BaumerSensor.cpp - wrapper around the Baumer OX-Line C++ SDK (libOXApi.so)
#include <BaumerSensor/BaumerSensor.h>

#include <OXApi/Ox.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace baumer {

namespace {

using Clock = std::chrono::steady_clock;

double nowSeconds() {
    static const auto t0 = Clock::now();
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

/// Counts events and publishes events/second every ~0.5 s.
class FpsCounter {
public:
    // worker thread
    void tick() {
        const double now = nowSeconds();
        double ws = windowStart_.load();
        if (ws < 0.0) {
            ws = now;
            windowStart_.store(ws);
        }
        ++count_;
        const double dt = now - ws;
        if (dt >= 0.5) {
            fps_.store(count_ / dt);
            count_ = 0;
            windowStart_.store(now);
        }
    }
    // only while the worker is not running
    void reset() {
        count_ = 0;
        windowStart_.store(-1.0);
        fps_.store(0.0);
    }
    // any thread
    double fps() const {
        // no events for a while -> show 0 instead of the last value
        const double ws = windowStart_.load();
        return (ws >= 0.0 && nowSeconds() - ws > 2.0) ? 0.0 : fps_.load();
    }

private:
    std::uint64_t count_ = 0;  // worker thread only
    std::atomic<double> windowStart_{-1.0};
    std::atomic<double> fps_{0.0};
};

}  // namespace

// ------------------------------------------------------------------------------------------------
struct BaumerSensor::Impl {
    SensorConfig cfg;

    // ---- SDK objects
    std::unique_ptr<Baumer::OXApi::Ox> ox;
    std::string oxHost;
    std::uint16_t oxPort = 0;
    std::shared_ptr<Baumer::OXApi::UdpStreaming::OxStream> stream;
    std::mutex ctrlMu;  // serialises every call on `ox` (control connection)
    std::atomic<bool> connected{false};
    bool loggedIn = false;  // guarded by ctrlMu

    // ---- workers
    std::thread profThread;
    std::atomic<bool> profRun{false};
    std::string profSource;  // "poll" | "udp" | ""
    std::thread imgThread;
    std::atomic<bool> imgRun{false};

    // ---- results
    std::mutex dataMu;
    ProfileFrame profile;
    std::uint64_t profileSeq = 0;
    ImageFrame image;
    std::uint64_t imageSeq = 0;

    // ---- info / stats
    mutable std::mutex infoMu;
    SensorInfo sensorInfo;
    std::atomic<double> precision{100.0};
    std::atomic<std::uint64_t> profilesReceived{0}, imagesReceived{0}, packetErrors{0};
    FpsCounter profFps, imgFps;

    // ---- log
    std::mutex logMu;
    std::vector<std::string> logLines;
    double lastThrottled = -10.0;
    std::uint64_t suppressed = 0;

    void log(const std::string& s) {
        std::lock_guard<std::mutex> lk(logMu);
        if (logLines.size() < 2000) logLines.push_back(s);
    }
    /// At most one message per 0.5 s, the others are counted.
    void logThrottled(const std::string& s) {
        std::lock_guard<std::mutex> lk(logMu);
        const double now = nowSeconds();
        if (now - lastThrottled < 0.5) {
            ++suppressed;
            return;
        }
        lastThrottled = now;
        std::string line = s;
        if (suppressed) line += " (+" + std::to_string(suppressed) + " more)";
        suppressed = 0;
        if (logLines.size() < 2000) logLines.push_back(line);
    }

    // Creates the Ox object (no network traffic). Re-creates it if host/port changed.
    void ensureOx() {
        if (!ox || oxHost != cfg.host || oxPort != cfg.udpPort) {
            ox = Baumer::OXApi::Ox::Create(cfg.host, cfg.udpPort);
            oxHost = cfg.host;
            oxPort = cfg.udpPort;
            loggedIn = false;
        }
    }

    // caller holds ctrlMu. Login is only needed for GetImage and for changing settings.
    void ensureLogin() {
        if (!loggedIn) {
            ox->Login("admin");
            loggedIn = true;
        }
    }

    void storeProfile(ProfileFrame&& f) {
        {
            std::lock_guard<std::mutex> lk(dataMu);
            profile = std::move(f);
            ++profileSeq;
        }
        ++profilesReceived;
        profFps.tick();
    }

    void pollLoop(double hz);
    void udpLoop();
    void imageLoop(double hz);
    void stopProfileLocked();
    void stopImageLocked();
};

// ---- profile polling (control connection) ------------------------------------------------------
void BaumerSensor::Impl::pollLoop(double hz) {
    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
    auto next = Clock::now();
    int failures = 0;
    while (profRun) {
        try {
            Baumer::OXApi::Types::IntensityProfile p = [&] {
                std::lock_guard<std::mutex> lk(ctrlMu);
                return ox->GetIntensityProfile();
            }();
            double prec = p.Precision > 0 ? static_cast<double>(p.Precision) : precision.load();
            precision.store(prec);
            ProfileFrame f;
            f.timestamp = p.TimeStamp;
            f.quality = p.QualityId;
            f.zValid = true;
            f.intensityValid = true;
            f.hostTime = nowSeconds();
            const std::size_t n = std::min({static_cast<std::size_t>(p.Length), p.X.size(), p.Z.size(), p.I.size()});
            f.x.resize(n);
            f.z.resize(n);
            f.intensity.resize(n);
            for (std::size_t i = 0; i < n; ++i) {
                // SDK documentation: x = (X + XStart) / Precision, z = Z / Precision
                f.x[i] = (static_cast<double>(p.X[i]) + p.XStart) / prec;
                f.z[i] = static_cast<double>(p.Z[i]) / prec;
                f.intensity[i] = static_cast<std::uint16_t>(std::clamp<std::int32_t>(p.I[i], 0, 65535));
            }
            storeProfile(std::move(f));
            failures = 0;
        } catch (const std::exception& e) {
            logThrottled(std::string("profile poll error: ") + e.what());
            if (++failures >= 10) {
                log("profile polling stopped after 10 consecutive errors");
                profRun = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        next += period;
        if (next < Clock::now()) next = Clock::now();  // we are behind: do not burst
        std::this_thread::sleep_until(next);
    }
}

// ---- profile UDP stream --------------------------------------------------------------------------
void BaumerSensor::Impl::udpLoop() {
    auto& st = *stream;
    while (profRun) {
        bool any = false;
        try {
            while (profRun && st.ProfileAvailable()) {
                const auto p = st.ReadProfile();
                any = true;
                ProfileFrame f;
                f.blockId = p.BlockId;
                f.timestamp = p.TimeStamp;
                f.quality = p.Quality;
                f.measurementRateHz = p.MeasurementRateHz;
                f.encoder = p.Encoder;
                f.valuesValid = p.ValuesValid;
                f.alarm = p.Alarm;
                f.hostTime = nowSeconds();
                const double prec = precision.load();
                const std::size_t n = std::min(static_cast<std::size_t>(p.Length), p.X.size());
                f.x.resize(n);
                for (std::size_t i = 0; i < n; ++i) f.x[i] = static_cast<double>(p.X[i]) / prec;
                if (p.ZValid && p.Z.size() >= n) {
                    f.zValid = true;
                    f.z.resize(n);
                    for (std::size_t i = 0; i < n; ++i) f.z[i] = static_cast<double>(p.Z[i]) / prec;
                }
                if (p.IntensityValid && p.I.size() >= n) {
                    f.intensityValid = true;
                    f.intensity.assign(p.I.begin(), p.I.begin() + static_cast<std::ptrdiff_t>(n));
                }
                storeProfile(std::move(f));
            }
            while (st.ErrorOccured()) {
                const auto e = st.ReadError();
                ++packetErrors;
                logThrottled("udp: " + e.Message + " (block " + std::to_string(e.BlockId) + ")");
            }
        } catch (const std::exception& e) {
            logThrottled(std::string("udp error: ") + e.what());
        }
        if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ---- camera image (control connection) -----------------------------------------------------------
void BaumerSensor::Impl::imageLoop(double hz) {
    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
    auto next = Clock::now();
    int failures = 0;
    while (imgRun) {
        try {
            Baumer::OXApi::Types::Image img = [&] {
                std::lock_guard<std::mutex> lk(ctrlMu);
                ensureLogin();
                return ox->GetImage();
            }();
            ImageFrame f;
            f.width = img.RoiWidth;
            f.height = img.RoiHeight;
            f.columnOffset = img.ColumnOffset;
            f.rowOffset = img.RowOffset;
            f.columnBinning = img.ColumnBinning;
            f.rowBinning = img.RowBinning;
            f.hostTime = nowSeconds();
            const std::size_t px = img.Pixels.size();
            if (px != static_cast<std::size_t>(f.width) * f.height) {
                // the SDK does not say whether Roi* is before or after binning: accept either
                const std::uint32_t cb = std::max<std::uint32_t>(1, img.ColumnBinning);
                const std::uint32_t rb = std::max<std::uint32_t>(1, img.RowBinning);
                if (px == static_cast<std::size_t>(f.width / cb) * (f.height / rb)) {
                    f.width /= cb;
                    f.height /= rb;
                } else {
                    throw std::runtime_error("image size mismatch: " + std::to_string(px) + " pixels for " +
                                             std::to_string(f.width) + "x" + std::to_string(f.height));
                }
            }
            f.pixels = img.Pixels;
            {
                std::lock_guard<std::mutex> lk(dataMu);
                image = std::move(f);
                ++imageSeq;
            }
            ++imagesReceived;
            imgFps.tick();
            failures = 0;
        } catch (const std::exception& e) {
            logThrottled(std::string("image error: ") + e.what());
            if (++failures >= 10) {
                log("image acquisition stopped after 10 consecutive errors");
                imgRun = false;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        next += period;
        if (next < Clock::now()) next = Clock::now();
        std::this_thread::sleep_until(next);
    }
}

void BaumerSensor::Impl::stopProfileLocked() {
    profRun = false;
    if (profThread.joinable()) profThread.join();
    if (stream) {
        try {
            stream->Stop();
            stream->Close();
        } catch (const std::exception& e) {
            log(std::string("stream close: ") + e.what());
        }
        stream.reset();
    }
    profSource.clear();
}

void BaumerSensor::Impl::stopImageLocked() {
    imgRun = false;
    if (imgThread.joinable()) imgThread.join();
}

// ------------------------------------------------------------------------------------------------
BaumerSensor::BaumerSensor() : d_(std::make_unique<Impl>()) {}

BaumerSensor::~BaumerSensor() {
    try {
        disconnect();
        stopProfile();
    } catch (...) {
    }
}

void BaumerSensor::setConfig(const SensorConfig& cfg) {
    d_->cfg = cfg;
    d_->precision.store(cfg.precision > 0 ? cfg.precision : 100.0);
}

bool BaumerSensor::isConnected() const { return d_->connected; }

SensorInfo BaumerSensor::info() const {
    std::lock_guard<std::mutex> lk(d_->infoMu);
    return d_->sensorInfo;
}

bool BaumerSensor::connect(std::string& err) {
    if (d_->connected) return true;
    try {
        std::lock_guard<std::mutex> lk(d_->ctrlMu);
        d_->ensureOx();
        d_->ox->Connect();
        d_->connected = true;
        d_->loggedIn = false;
    } catch (const std::exception& e) {
        err = e.what();
        d_->log("connect failed: " + err);
        return false;
    }

    // Read sensor information. Every item is optional: a failure here must not prevent use.
    SensorInfo si;
    {
        std::lock_guard<std::mutex> lk(d_->ctrlMu);
        try {
            const auto s = d_->ox->GetSensorInfo();
            si.type = s.Type;
            si.serialNumber = s.SerialNumber;
            si.vendor = s.VendorName;
            si.aggregateVersion = s.AggregateVersion;
            si.softwareVersion = s.SoftwareVersion;
        } catch (const std::exception& e) {
            d_->log(std::string("GetSensorInfo: ") + e.what());
        }
        try {
            const auto pi = d_->ox->GetProfileInfo();
            si.xUnit = pi.XUnit;
            si.zUnit = pi.ZUnit;
            si.profileMaxLength = pi.MaxLength;
        } catch (const std::exception& e) {
            d_->log(std::string("GetProfileInfo: ") + e.what());
        }
        try {
            const auto ii = d_->ox->GetImageInfo();
            si.imageWidth = ii.SensorWidth;
            si.imageHeight = ii.SensorHeight;
        } catch (const std::exception& e) {
            d_->log(std::string("GetImageInfo: ") + e.what());
        }
        try {
            const auto p = d_->ox->GetProfile();
            if (p.Precision > 0) {
                si.precision = static_cast<double>(p.Precision);
                d_->precision.store(si.precision);
            }
        } catch (const std::exception& e) {
            d_->log(std::string("could not read precision (") + e.what() + "), using " +
                    std::to_string(d_->precision.load()));
        }
    }
    {
        std::lock_guard<std::mutex> lk(d_->infoMu);
        d_->sensorInfo = si;
    }
    d_->log("connected to " + d_->cfg.host + " (" + si.type + ", S/N " + si.serialNumber + ", precision " +
            std::to_string(d_->precision.load()) + ")");
    return true;
}

void BaumerSensor::disconnect() {
    if (!d_->connected) return;
    d_->stopImageLocked();
    if (d_->profSource == "poll") d_->stopProfileLocked();
    try {
        std::lock_guard<std::mutex> lk(d_->ctrlMu);
        d_->ox->Disconnect();
    } catch (const std::exception& e) {
        d_->log(std::string("disconnect: ") + e.what());
    }
    d_->connected = false;
    d_->loggedIn = false;
    d_->log("disconnected");
}

bool BaumerSensor::startPolling(double hz, std::string& err) {
    if (!d_->connected) {
        err = "not connected";
        return false;
    }
    hz = std::clamp(hz, 0.1, 100.0);
    stopProfile();
    d_->profFps.reset();
    d_->profRun = true;
    d_->profSource = "poll";
    d_->profThread = std::thread([this, hz] { d_->pollLoop(hz); });
    return true;
}

bool BaumerSensor::startUdp(std::string& err) {
    stopProfile();
    try {
        {
            std::lock_guard<std::mutex> lk(d_->ctrlMu);
            d_->ensureOx();
            d_->stream = d_->ox->CreateStream();
        }
        // the sensor sends bursts of fragmented packets: use a large socket buffer
        d_->stream->SetReceiveBufferSize(4 * 1024 * 1024);
        d_->stream->Start();
    } catch (const std::exception& e) {
        err = e.what();
        d_->stream.reset();
        d_->log("udp start failed: " + err);
        return false;
    }
    d_->profFps.reset();
    d_->profRun = true;
    d_->profSource = "udp";
    d_->profThread = std::thread([this] { d_->udpLoop(); });
    d_->log("listening for UDP profiles on port " + std::to_string(d_->cfg.udpPort));
    return true;
}

void BaumerSensor::stopProfile() { d_->stopProfileLocked(); }

bool BaumerSensor::startImage(double hz, std::string& err) {
    if (!d_->connected) {
        err = "not connected";
        return false;
    }
    hz = std::clamp(hz, 0.1, 30.0);
    stopImage();
    d_->imgFps.reset();
    d_->imgRun = true;
    d_->imgThread = std::thread([this, hz] { d_->imageLoop(hz); });
    return true;
}

void BaumerSensor::stopImage() { d_->stopImageLocked(); }

bool BaumerSensor::getUdpStreamOptions(std::vector<UdpStreamOption>& out, std::string& err) {
    if (!d_->connected) {
        err = "not connected";
        return false;
    }
    try {
        std::lock_guard<std::mutex> lk(d_->ctrlMu);
        const auto info = d_->ox->GetUdpStreamingInfo();
        const auto active = d_->ox->GetActiveUdpStreams();
        out.clear();
        for (const auto& s : info.UdpStreams) {
            UdpStreamOption o;
            o.id = s.Id;
            o.name = s.Name;
            o.active = std::find(active.begin(), active.end(), s.Id) != active.end();
            out.push_back(std::move(o));
        }
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

bool BaumerSensor::applyUdpStreams(const std::string& localIp, const std::vector<std::uint32_t>& streamIds,
                                   std::string& err) {
    if (!d_->connected) {
        err = "not connected";
        return false;
    }
    try {
        std::lock_guard<std::mutex> lk(d_->ctrlMu);
        d_->ensureLogin();
        auto pi = d_->ox->GetProcessInterfaces();  // read-modify-write: keep Modbus/OPC UA/... as they are
        pi.UdpStreamingEnabled = !streamIds.empty();
        pi.UdpStreamingIp = localIp;
        pi.UdpStreamingPort = d_->cfg.udpPort;
        d_->ox->ConfigureProcessInterfaces(pi);
        d_->ox->ConfigureActiveUdpStreams(streamIds);
        d_->log("sensor now streams to " + localIp + ":" + std::to_string(d_->cfg.udpPort) + " (" +
                std::to_string(streamIds.size()) + " stream(s))");
    } catch (const std::exception& e) {
        err = e.what();
        d_->log("applyUdpStreams failed: " + err);
        return false;
    }
    return true;
}

bool BaumerSensor::latestProfile(ProfileFrame& out, std::uint64_t& seq) {
    std::lock_guard<std::mutex> lk(d_->dataMu);
    if (d_->profileSeq == seq) return false;
    out = d_->profile;
    seq = d_->profileSeq;
    return true;
}

bool BaumerSensor::latestImage(ImageFrame& out, std::uint64_t& seq) {
    std::lock_guard<std::mutex> lk(d_->dataMu);
    if (d_->imageSeq == seq) return false;
    out = d_->image;
    seq = d_->imageSeq;
    return true;
}

SensorStats BaumerSensor::stats() const {
    SensorStats s;
    s.controlConnected = d_->connected;
    s.profileRunning = d_->profRun;
    s.imageRunning = d_->imgRun;
    s.profileSource = d_->profSource;
    s.profilesReceived = d_->profilesReceived;
    s.imagesReceived = d_->imagesReceived;
    s.packetErrors = d_->packetErrors;
    s.profileFps = d_->profFps.fps();
    s.imageFps = d_->imgFps.fps();
    s.precision = d_->precision;
    return s;
}

std::vector<std::string> BaumerSensor::drainLog() {
    std::lock_guard<std::mutex> lk(d_->logMu);
    std::vector<std::string> out;
    out.swap(d_->logLines);
    return out;
}

// ------------------------------------------------------------------------------------------------
std::string guessLocalIp(const std::string& sensorHost) {
    in_addr target{};
    if (inet_pton(AF_INET, sensorHost.c_str(), &target) != 1) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        addrinfo* res = nullptr;
        if (getaddrinfo(sensorHost.c_str(), nullptr, &hints, &res) != 0 || !res) return {};
        target = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return {};
    std::string found;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || !a->ifa_netmask || a->ifa_addr->sa_family != AF_INET) continue;
        const in_addr ip = reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr;
        const in_addr mask = reinterpret_cast<sockaddr_in*>(a->ifa_netmask)->sin_addr;
        if ((ip.s_addr & mask.s_addr) == (target.s_addr & mask.s_addr)) {
            char buf[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &ip, buf, sizeof buf);
            found = buf;
            break;
        }
    }
    freeifaddrs(list);
    return found;
}

}  // namespace baumer
