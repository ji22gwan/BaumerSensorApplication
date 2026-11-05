// Viewer.cpp
#include "Viewer.h"

#include <BaumerSensor/BaumerSensor.h>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>  // DockBuilder
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <implot.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <future>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace baumer {

namespace {

std::string timestampName() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", std::localtime(&t));
    return buf;
}

// classic "jet" colour map
void jet(std::uint8_t v, std::uint8_t* rgb) {
    const float x = v / 255.f;
    auto c = [](float f) { return static_cast<std::uint8_t>(255.f * std::clamp(f, 0.f, 1.f)); };
    rgb[0] = c(1.5f - std::fabs(4.f * x - 3.f));
    rgb[1] = c(1.5f - std::fabs(4.f * x - 2.f));
    rgb[2] = c(1.5f - std::fabs(4.f * x - 1.f));
}

template <std::size_t N>
void copyStr(char (&dst)[N], const std::string& s) {
    std::snprintf(dst, N, "%s", s.c_str());
}

}  // namespace

// ------------------------------------------------------------------------------------------------
struct Viewer::Impl {
    std::unique_ptr<SensorSource> src;
    ViewerOptions opt;
    GLFWwindow* window = nullptr;

    // ---- connection panel state
    char host[64] = "192.168.0.250";
    int udpPort = 12345;
    double precision = 100.0;
    int profileMode = 0;  // 0 = poll (control connection), 1 = UDP stream
    float pollHz = 10.f;
    float imageHz = 2.f;
    char localIp[48] = "";
    std::vector<UdpStreamOption> streamOptions;
    std::vector<char> streamChecked;
    bool autostartPending = false;
    bool connecting = false;
    std::future<std::pair<bool, std::string>> connectFut;

    // ---- data
    ProfileFrame prof;
    std::uint64_t profSeq = 0;
    ImageFrame img;
    std::uint64_t imgSeq = 0;
    bool frozen = false;

    // ---- display options
    bool autoFit = true, showPoints = true, showLine = true, hideZero = false, flipZ = false, equalAspect = false;
    bool falseColor = false, markSaturated = true;
    bool fitImage = true;

    // derived plot arrays
    std::vector<double> px, pz, ix, iv;

    // image texture
    unsigned int tex = 0;
    std::uint32_t texW = 0, texH = 0;
    std::vector<std::uint8_t> rgba;

    std::deque<std::string> logLines;
    bool scrollLog = true;
    bool layoutBuilt = false;
    std::string status;

    // ------------------------------------------------------------------------------------------
    void log(const std::string& s) {
        logLines.push_back(s);
        while (logLines.size() > 1000) logLines.pop_front();
    }

    SensorConfig currentConfig() const {
        SensorConfig c;
        c.host = host;
        c.udpPort = static_cast<std::uint16_t>(std::clamp(udpPort, 1, 65535));
        c.precision = precision > 0 ? precision : 100.0;
        return c;
    }

    void applyConfig() { src->setConfig(currentConfig()); }

    // The connection attempt can block for a long time (sensor off / wrong subnet), so it runs in the
    // background and the GUI stays responsive. pollConnect() picks up the result.
    void doConnect() {
        if (connecting) return;
        applyConfig();
        connecting = true;
        status = "connecting to " + std::string(host) + " ...";
        connectFut = std::async(std::launch::async, [this] {
            std::string e;
            const bool ok = src->connect(e);
            return std::make_pair(ok, e);
        });
    }

    void pollConnect() {
        using namespace std::chrono_literals;
        if (!connecting || connectFut.wait_for(0s) != std::future_status::ready) return;
        const auto [ok, err] = connectFut.get();
        connecting = false;
        if (!ok) {
            status = "connect failed: " + err;
            log(status);
            autostartPending = false;
            return;
        }
        status = "connected";
        const SensorInfo si = src->info();
        if (si.precision > 0) precision = si.precision;
        if (localIp[0] == '\0') copyStr(localIp, guessLocalIp(host));
        refreshStreams();
        if (autostartPending) {
            autostartPending = false;
            profileMode = 0;
            doStartProfile();
            doStartImage();
        }
    }

    void doDisconnect() {
        src->disconnect();
        status = "disconnected";
    }

    void doStartProfile() {
        std::string err;
        applyConfig();
        const bool ok = (profileMode == 0) ? src->startPolling(pollHz, err) : src->startUdp(err);
        if (!ok) {
            status = "start failed: " + err;
            log(status);
        } else {
            status = profileMode == 0 ? "polling profiles" : "receiving UDP profiles";
        }
    }

    void doStartImage() {
        std::string err;
        if (!src->startImage(imageHz, err)) {
            status = "image start failed: " + err;
            log(status);
        }
    }

    void refreshStreams() {
        std::string err;
        std::vector<UdpStreamOption> opts;
        if (!src->getUdpStreamOptions(opts, err)) {
            log("stream list: " + err);
            return;
        }
        streamOptions = opts;
        streamChecked.assign(opts.size(), 0);
        for (std::size_t i = 0; i < opts.size(); ++i) streamChecked[i] = opts[i].active ? 1 : 0;
        for (const auto& o : opts) log("sensor UDP stream " + std::to_string(o.id) + ": " + o.name + (o.active ? " [active]" : ""));
    }

    void applyStreams() {
        std::vector<std::uint32_t> ids;
        for (std::size_t i = 0; i < streamOptions.size(); ++i)
            if (streamChecked[i]) ids.push_back(streamOptions[i].id);
        std::string err;
        applyConfig();
        if (!src->applyUdpStreams(localIp, ids, err)) {
            status = "apply failed: " + err;
            log(status);
        } else {
            status = "sensor UDP target set";
            refreshStreams();
        }
    }

    // ------------------------------------------------------------------------------------------
    void pullData() {
        if (frozen) return;
        if (src->latestProfile(prof, profSeq)) rebuildPlotArrays();
        if (src->latestImage(img, imgSeq)) uploadImage();
        for (auto& l : src->drainLog()) log(l);
    }

    void rebuildPlotArrays() {
        px.clear();
        pz.clear();
        ix.clear();
        iv.clear();
        const std::size_t n = prof.x.size();
        if (prof.zValid && prof.z.size() >= n) {
            px.reserve(n);
            pz.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                if (hideZero && prof.z[i] == 0.0) continue;
                px.push_back(prof.x[i]);
                pz.push_back(prof.z[i]);
            }
        }
        if (prof.intensityValid && prof.intensity.size() >= n) {
            ix.reserve(n);
            iv.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                ix.push_back(prof.x[i]);
                iv.push_back(prof.intensity[i]);
            }
        }
    }

    void uploadImage() {
        if (img.width == 0 || img.height == 0 || img.pixels.size() < static_cast<std::size_t>(img.width) * img.height) return;
        const std::size_t n = static_cast<std::size_t>(img.width) * img.height;
        rgba.resize(n * 4);
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint8_t v = img.pixels[i];
            std::uint8_t* o = &rgba[i * 4];
            if (markSaturated && v == 255) {
                o[0] = 255; o[1] = 0; o[2] = 255;
            } else if (falseColor) {
                jet(v, o);
            } else {
                o[0] = o[1] = o[2] = v;
            }
            o[3] = 255;
        }
        if (!tex) glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (texW != img.width || texH != img.height) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(img.width), static_cast<GLsizei>(img.height), 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            texW = img.width;
            texH = img.height;
            fitImage = true;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(img.width), static_cast<GLsizei>(img.height),
                            GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        }
    }

    // ------------------------------------------------------------------------------------------
    void saveCsv() {
        const std::string name = "profile_" + timestampName() + ".csv";
        std::ofstream f(name);
        if (!f) {
            log("cannot write " + name);
            return;
        }
        f << "index,x_mm,z_mm,intensity\n";
        for (std::size_t i = 0; i < prof.x.size(); ++i) {
            f << i << ',' << prof.x[i] << ',';
            if (prof.zValid && i < prof.z.size()) f << prof.z[i];
            f << ',';
            if (prof.intensityValid && i < prof.intensity.size()) f << prof.intensity[i];
            f << '\n';
        }
        log("saved " + name);
    }

    void savePgm() {
        if (img.pixels.empty()) return;
        const std::string name = "image_" + timestampName() + ".pgm";
        std::ofstream f(name, std::ios::binary);
        if (!f) {
            log("cannot write " + name);
            return;
        }
        f << "P5\n" << img.width << ' ' << img.height << "\n255\n";
        f.write(reinterpret_cast<const char*>(img.pixels.data()), static_cast<std::streamsize>(img.pixels.size()));
        log("saved " + name + " (raw 8 bit, no color map)");
    }

    // ------------------------------------------------------------------------------------------
    void buildLayout(ImGuiID dock, const ImVec2& size) {
        ImGui::DockBuilderRemoveNode(dock);
        ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dock, size);
        ImGuiID rest = dock, bottom, left, right, centerBottom;
        bottom = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Down, 0.26f, nullptr, &rest);
        left = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Left, 0.20f, nullptr, &rest);
        right = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, 0.34f, nullptr, &rest);
        centerBottom = ImGui::DockBuilderSplitNode(rest, ImGuiDir_Down, 0.38f, nullptr, &rest);
        ImGui::DockBuilderDockWindow("Connection", left);
        ImGui::DockBuilderDockWindow("Profile (X-Z)", rest);
        ImGui::DockBuilderDockWindow("Intensity", centerBottom);
        ImGui::DockBuilderDockWindow("Camera Image", right);
        ImGui::DockBuilderDockWindow("Log", bottom);
        ImGui::DockBuilderDockWindow("Raw Data", bottom);  // docked last -> the visible tab
        ImGui::DockBuilderFinish(dock);
        if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(bottom)) n->SelectedTabId = ImHashStr("Raw Data");
    }

    void drawConnection() {
        ImGui::Begin("Connection");
        const SensorStats st = src->stats();
        const bool connected = src->isConnected();
        const bool locked = connected || connecting || st.profileRunning;

        ImGui::BeginDisabled(locked);
        ImGui::SetNextItemWidth(-90);
        ImGui::InputText("Sensor IP", host, sizeof host);
        ImGui::SetNextItemWidth(-90);
        ImGui::InputInt("UDP port", &udpPort, 0);
        ImGui::SetNextItemWidth(-90);
        ImGui::InputDouble("Precision", &precision, 0.0, 0.0, "%.0f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("raw value / precision = mm. Read from the sensor on connect.");
        ImGui::EndDisabled();

        if (connecting) {
            ImGui::BeginDisabled();
            ImGui::Button("Connecting ...", ImVec2(-1, 0));
            ImGui::EndDisabled();
        } else if (!connected) {
            if (ImGui::Button("Connect", ImVec2(-1, 0))) doConnect();
        } else {
            if (ImGui::Button("Disconnect", ImVec2(-1, 0))) doDisconnect();
        }
        ImGui::TextColored(connected ? ImVec4(0.4f, 1.f, 0.4f, 1.f) : ImVec4(0.7f, 0.7f, 0.7f, 1.f), "%s",
                           connected ? "control connection: ON" : "control connection: off");
        if (connected) {
            const SensorInfo si = src->info();
            ImGui::TextDisabled("%s  S/N %s", si.type.c_str(), si.serialNumber.c_str());
            ImGui::TextDisabled("FW %s  image %ux%u", si.softwareVersion.c_str(), si.imageWidth, si.imageHeight);
        }

        ImGui::SeparatorText("Profile");
        ImGui::BeginDisabled(st.profileRunning);
        ImGui::RadioButton("Poll (control connection)", &profileMode, 0);
        ImGui::RadioButton("UDP stream", &profileMode, 1);
        ImGui::EndDisabled();
        if (profileMode == 0) {
            ImGui::SetNextItemWidth(-90);
            ImGui::SliderFloat("Poll Hz", &pollHz, 1.f, 50.f, "%.0f");
        }
        if (!st.profileRunning) {
            ImGui::BeginDisabled(profileMode == 0 && !connected);
            if (ImGui::Button("Start profile", ImVec2(-1, 0))) doStartProfile();
            ImGui::EndDisabled();
        } else if (ImGui::Button("Stop profile", ImVec2(-1, 0))) {
            src->stopProfile();
            status = "profile stopped";
        }

        ImGui::SeparatorText("Camera image");
        ImGui::SetNextItemWidth(-90);
        ImGui::SliderFloat("Image Hz", &imageHz, 0.5f, 10.f, "%.1f");
        if (!st.imageRunning) {
            ImGui::BeginDisabled(!connected);
            if (ImGui::Button("Start image", ImVec2(-1, 0))) doStartImage();
            ImGui::EndDisabled();
        } else if (ImGui::Button("Stop image", ImVec2(-1, 0))) {
            src->stopImage();
        }

        if (ImGui::CollapsingHeader("UDP target (sensor setup)")) {
            ImGui::TextWrapped("Writes sensor settings: tells the sensor which PC/port to stream to.");
            ImGui::SetNextItemWidth(-90);
            ImGui::InputText("This PC IP", localIp, sizeof localIp);
            ImGui::BeginDisabled(!connected);
            if (ImGui::Button("Refresh streams")) refreshStreams();
            for (std::size_t i = 0; i < streamOptions.size(); ++i) {
                bool c = streamChecked[i] != 0;
                const std::string label = std::to_string(streamOptions[i].id) + ": " + streamOptions[i].name;
                if (ImGui::Checkbox(label.c_str(), &c)) streamChecked[i] = c ? 1 : 0;
            }
            if (ImGui::Button("Apply to sensor", ImVec2(-1, 0))) applyStreams();
            ImGui::EndDisabled();
        }

        ImGui::SeparatorText("Statistics");
        ImGui::Text("profiles: %llu  (%.1f /s)", static_cast<unsigned long long>(st.profilesReceived), st.profileFps);
        ImGui::Text("images:   %llu  (%.1f /s)", static_cast<unsigned long long>(st.imagesReceived), st.imageFps);
        ImGui::Text("UDP errors: %llu", static_cast<unsigned long long>(st.packetErrors));
        ImGui::Text("precision in use: %.0f", st.precision);
        ImGui::Checkbox("Freeze display", &frozen);
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::End();
    }

    void drawProfile() {
        ImGui::Begin("Profile (X-Z)");
        ImGui::Checkbox("Auto-fit", &autoFit);
        ImGui::SameLine();
        ImGui::Checkbox("Points", &showPoints);
        ImGui::SameLine();
        ImGui::Checkbox("Line", &showLine);
        ImGui::SameLine();
        ImGui::Checkbox("Flip Z", &flipZ);
        ImGui::SameLine();
        ImGui::Checkbox("Equal aspect", &equalAspect);
        ImGui::SameLine();
        if (ImGui::Checkbox("Hide Z=0", &hideZero)) rebuildPlotArrays();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hide points with raw Z == 0 (assumed invalid)");
        ImGui::Text("points %zu | block %u | quality %u | rate %.0f Hz | valid %d alarm %d | ts %llu", prof.x.size(),
                    prof.blockId, prof.quality, prof.measurementRateHz, prof.valuesValid, prof.alarm,
                    static_cast<unsigned long long>(prof.timestamp));

        const ImPlotAxisFlags fit = autoFit ? ImPlotAxisFlags_AutoFit : 0;
        if (ImPlot::BeginPlot("##profile", ImVec2(-1, -1), equalAspect ? ImPlotFlags_Equal : 0)) {
            ImPlot::SetupAxes("X [mm]", "Z [mm]", fit, fit | (flipZ ? ImPlotAxisFlags_Invert : 0));
            if (!px.empty()) {
                if (showPoints) {
                    ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 2.5f);
                    ImPlot::PlotScatter("Z", px.data(), pz.data(), static_cast<int>(px.size()));
                }
                if (showLine) ImPlot::PlotLine("Z (line)", px.data(), pz.data(), static_cast<int>(px.size()));
            }
            ImPlot::EndPlot();
        }
        ImGui::End();
    }

    void drawIntensity() {
        ImGui::Begin("Intensity");
        if (ix.empty())
            ImGui::TextDisabled("no intensity data (needs a Z-I / I profile UDP stream, or polling)");
        const ImPlotAxisFlags fit = autoFit ? ImPlotAxisFlags_AutoFit : 0;
        if (ImPlot::BeginPlot("##intensity", ImVec2(-1, -1))) {
            ImPlot::SetupAxes("X [mm]", "Intensity [raw]", fit, fit);
            if (!ix.empty()) ImPlot::PlotLine("I", ix.data(), iv.data(), static_cast<int>(ix.size()));
            ImPlot::EndPlot();
        }
        ImGui::End();
    }

    void drawImage() {
        ImGui::Begin("Camera Image");
        if (ImGui::Checkbox("False color", &falseColor)) uploadImage();
        ImGui::SameLine();
        if (ImGui::Checkbox("Mark saturated", &markSaturated)) uploadImage();
        ImGui::SameLine();
        if (ImGui::Button("Fit")) fitImage = true;
        ImGui::SameLine();
        if (ImGui::Button("Save PGM")) savePgm();
        if (tex && texW) {
            ImGui::Text("ROI %ux%u @ (%u,%u)  binning %ux%u", img.width, img.height, img.columnOffset, img.rowOffset,
                        img.columnBinning, img.rowBinning);
        } else {
            ImGui::TextDisabled("no image yet - connect and press \"Start image\"");
        }
        if (ImPlot::BeginPlot("##image", ImVec2(-1, -1), ImPlotFlags_Equal | ImPlotFlags_NoLegend)) {
            ImPlot::SetupAxes("column [px]", "row [px]", 0, ImPlotAxisFlags_Invert);
            if (tex && texW && texH) {
                ImPlot::SetupAxesLimits(0, texW, 0, texH, fitImage ? ImPlotCond_Always : ImPlotCond_Once);
                fitImage = false;
                // Y axis is inverted (row 0 on top); uv is flipped so that texture row 0 lands on image row 0
                ImPlot::PlotImage("##img", static_cast<ImTextureID>(static_cast<std::uintptr_t>(tex)), ImPlotPoint(0, 0),
                                  ImPlotPoint(texW, texH), ImVec2(0, 1), ImVec2(1, 0));
            }
            ImPlot::EndPlot();
        }
        ImGui::End();
    }

    void drawRaw() {
        ImGui::Begin("Raw Data");
        if (ImGui::Button("Save profile CSV")) saveCsv();
        ImGui::SameLine();
        ImGui::TextDisabled("%zu points  (units: mm, intensity raw)", prof.x.size());
        if (ImGui::BeginTable("raw", 4,
                              ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("#");
            ImGui::TableSetupColumn("X [mm]");
            ImGui::TableSetupColumn("Z [mm]");
            ImGui::TableSetupColumn("Intensity");
            ImGui::TableHeadersRow();
            ImGuiListClipper clip;
            clip.Begin(static_cast<int>(prof.x.size()));
            while (clip.Step()) {
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%d", i);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.4f", prof.x[i]);
                    ImGui::TableSetColumnIndex(2);
                    if (prof.zValid && static_cast<std::size_t>(i) < prof.z.size()) ImGui::Text("%.4f", prof.z[i]);
                    ImGui::TableSetColumnIndex(3);
                    if (prof.intensityValid && static_cast<std::size_t>(i) < prof.intensity.size())
                        ImGui::Text("%u", prof.intensity[i]);
                }
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }

    void drawLog() {
        ImGui::Begin("Log");
        if (ImGui::Button("Clear")) logLines.clear();
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &scrollLog);
        ImGui::BeginChild("logscroll", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& l : logLines) ImGui::TextUnformatted(l.c_str());
        if (scrollLog && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 5.f) ImGui::SetScrollHereY(1.f);
        ImGui::EndChild();
        ImGui::End();
    }
};

// ------------------------------------------------------------------------------------------------
Viewer::Viewer(std::unique_ptr<SensorSource> source, ViewerOptions opt) : d_(std::make_unique<Impl>()) {
    d_->src = std::move(source);
    d_->opt = std::move(opt);
    copyStr(d_->host, d_->opt.cfg.host);
    d_->udpPort = d_->opt.cfg.udpPort;
    d_->precision = d_->opt.cfg.precision;
    d_->autostartPending = d_->opt.autostart;
}

Viewer::~Viewer() = default;

int Viewer::run() {
    Impl& d = *d_;
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed (no display?)\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    d.window = glfwCreateWindow(1600, 900, d.opt.demo ? "BaumerSensorApplication [DEMO]" : "BaumerSensorApplication",
                                nullptr, nullptr);
    if (!d.window) {
        std::fprintf(stderr, "cannot create OpenGL window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(d.window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // deterministic layout, nothing written next to the binary
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.f;  // required for detached (viewport) windows
    style.Colors[ImGuiCol_WindowBg].w = 1.f;

    float scale = d.opt.uiScale;
    if (scale <= 0.f) {
        float sx = 1.f, sy = 1.f;
        glfwGetWindowContentScale(d.window, &sx, &sy);
        scale = std::max(sx, 1.f);
    }
    style.ScaleAllSizes(scale);
    io.FontGlobalScale = scale;

    ImGui_ImplGlfw_InitForOpenGL(d.window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    if (d.autostartPending) {
        d.doConnect();  // continues in pollConnect()
    } else if (d.opt.startUdp) {
        // UDP only: no control connection needed (sensor already configured to stream to this PC)
        d.profileMode = 1;
        d.doStartProfile();
    }

    int frame = 0;
    while (!glfwWindowShouldClose(d.window)) {
        glfwPollEvents();
        d.pollConnect();
        d.pullData();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImGuiID dockId = ImHashStr("BaumerDockSpace");
        if (!d.layoutBuilt) {
            d.buildLayout(dockId, vp->WorkSize);
            d.layoutBuilt = true;
        }
        ImGui::DockSpaceOverViewport(dockId, vp);

        d.drawConnection();
        d.drawProfile();
        d.drawIntensity();
        d.drawImage();
        d.drawLog();
        d.drawRaw();  // submitted last: becomes the selected tab of the bottom dock node

        ImGui::Render();
        int w = 0, h = 0;
        glfwGetFramebufferSize(d.window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.08f, 0.08f, 0.09f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        if (!d.opt.screenshotPath.empty() && d.opt.exitAfterFrames > 0 && frame == d.opt.exitAfterFrames - 1) {
            std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 3);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
            std::ofstream f(d.opt.screenshotPath, std::ios::binary);
            f << "P6\n" << w << ' ' << h << "\n255\n";
            for (int y = h - 1; y >= 0; --y)  // GL origin is bottom-left
                f.write(reinterpret_cast<const char*>(&px[static_cast<std::size_t>(y) * w * 3]), static_cast<std::streamsize>(w) * 3);
        }

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            GLFWwindow* backup = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backup);
        }
        glfwSwapBuffers(d.window);

        if (d.opt.exitAfterFrames > 0 && ++frame >= d.opt.exitAfterFrames) break;
    }

    if (d.connecting) d.connectFut.wait();  // cannot be cancelled; returns when the SDK gives up
    d.src->stopImage();
    d.src->stopProfile();
    d.src->disconnect();

    if (d.tex) glDeleteTextures(1, &d.tex);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(d.window);
    glfwTerminate();
    return 0;
}

}  // namespace baumer
