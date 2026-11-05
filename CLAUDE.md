# BaumerSensorApplication - context for Claude Code

Read this first. It carries the state of the project from the cloud session that created it, so work can
continue in VS Code without re-deriving anything. Keep it up to date when you change something that matters.

## What this is

Standalone test tool + embeddable library for the **Baumer OXP200-R10C.005** (OX200 family, 2D laser-triangulation
profile sensor, red 660 nm, measuring range 100 mm, working range 150..250 mm, raw profile output, no on-sensor tools).

- **Goal 1 (done):** standalone C++ app with a viewer showing the sensor's raw profile data and, in a separate
  window, the camera image.
- **Goal 2 (next):** the same code is later linked as a **library into another controller**. That is why the
  sensor code lives in `lib/` (`baumer_sensor`) with public headers that do not expose the Baumer SDK.
- Owner works at Hanwha Ocean on robotics/automation for shipyard use (likely use case: weld seam / groove
  measurement on a robot). Talk to the owner in **Korean**; code, identifiers and UI strings are English.

## Build / test / run

```bash
sudo apt install build-essential cmake libglfw3-dev libgl1-mesa-dev
cmake -S . -B build && cmake --build build -j
ctest --test-dir build                                   # udp_loopback, needs no sensor
./build/BaumerSensorApplication --demo --autostart       # synthetic profile + image
./build/ox_udp_sim --hz 100 & ./build/BaumerSensorApplication --udp   # fake UDP stream -> real receive code
./build/BaumerSensorApplication --host 192.168.0.250 --autostart      # real sensor (poll + image)
```
Headless GUI check (what was used to verify the viewer in the cloud session):
`xvfb-run -a -s "-screen 0 1700x960x24" ./build/BaumerSensorApplication --demo --autostart --scale 1 --frames 150 --screenshot out.ppm`

## Layout

| Path | Purpose |
|---|---|
| `lib/include/BaumerSensor/` | `Types.h` (plain structs), `SensorSource.h` (abstract interface), `BaumerSensor.h` (real impl, pimpl, + `guessLocalIp`) |
| `lib/src/BaumerSensor.cpp` | wrapper around `libOXApi.so`; worker threads for UDP / polling / image |
| `app/` | `Viewer.{h,cpp}` (ImGui+ImPlot+GLFW), `DemoSensor.*` (synthetic `SensorSource`), `main.cpp` (CLI) |
| `tools/OxUdpSimulator.h`, `ox_udp_sim.cpp` | sends OX200 UDP packets per manual; the packet layout is documented in the header |
| `tests/udp_loopback_test.cpp` | simulator -> real libOXApi parser -> `BaumerSensor`; asserts values incl. fragmented packets |
| `third_party/OXApi` | Baumer OX-SDK 2.0.4 (headers, `libOXApi.so` for amd64/arm64/armhf/i386, EULA, SDK readme, `oxapiexamples_reference.cpp` = Baumer's own C++ example) |
| `third_party/imgui`, `implot` | vendored (no network needed to build) |

Design rule: the Viewer only talks to `SensorSource`, so it runs on `BaumerSensor` (real) or `DemoSensor`.
`BaumerSensor` public headers must stay free of Baumer SDK includes (checked by compiling a TU with only `lib/include`).

## Facts about the sensor / SDK (verified from the SDK headers, example code and the OX200 manual FW 1.7)

- Data paths: **UDP stream** (profile Z / I / Z-I, 16 or 32 bit; parsed inside `libOXApi.so` by `OxStream`) and the
  **control connection** (websocket): `GetProfile`, `GetIntensityProfile`, **`GetImage`** (8 bit raw camera ROI), config calls.
  The camera image is only available over the control connection, not via UDP.
- Units: mm = raw / `Precision`. Polling: `x = (X + XStart) / Precision`, `z = Z / Precision`. UDP: `x = X / Precision`
  (as in Baumer's Python example). `Precision` comes from `GetProfile()`; UDP-only use falls back to 100 (`--precision`).
- **Only one control connection at a time**: if the sensor web interface is open, `Ox::Connect()` fails.
- `OxStream` works without `Connect()` (the SDK example never connects for streaming). `Ox::Create(host, udpPort)` binds nothing by itself.
- `GetImage`/config writes need `Login("admin")` (empty password); profile reading does not. We log in lazily.
- UDP stream ids/names come from `GetUdpStreamingInfo()`; we do not guess them, the UI lists them as checkboxes.
- Default sensor IP 192.168.0.250, default UDP port 12345 (SDK example) / 1234 (Ox::Create default). We use 12345.
- `libOXApi.so` needs glibc >= 2.30 (Ubuntu >= 20.04); no Boost needed in this SDK version (the old manual mentions Boost).
- Z invalid-value marker in UDP is **not documented** in the manual. `ProfilePacket` has `ZValid`/`IntensityValid` flags; we
  show raw values and offer `Hide Z=0`. Check on real hardware what invalid points look like and handle it properly.
- Timestamps are passed through raw (unit not verified). FPS in the UI is measured on the PC.

## Status

Verified (cloud sandbox, no sensor):
- UDP receive path (single + fragmented packets, Z / I / Z-I) via simulator + real `libOXApi.so`, `ctest` green.
- Viewer GUI in `--demo` and `--udp` + simulator modes (screenshots via Xvfb); image orientation; connect-failure handling.

**NOT verified on a real sensor yet** (written after Baumer's SDK example, expect small surprises):
1. `connect()` incl. `GetSensorInfo / GetProfileInfo / GetImageInfo / GetProfile` (precision).
2. Profile polling via `GetIntensityProfile` (rate, content, Z/I validity).
3. `GetImage` (`Roi*` vs binning vs `Pixels.size()` - code accepts either width/height convention, see `imageLoop`).
4. `applyUdpStreams` (read-modify-write of `ProcessInterfaces`, then `ConfigureActiveUdpStreams`) and the real UDP stream
   (precision, x/z scaling, `ProfilePacket.Z` semantics, packet loss at high rates / socket buffer size).
5. Behaviour of `Connect()` against a host that is routable but silent (the sandbox fails instantly; real LAN may block).
   Connect already runs in the background so the GUI never freezes; closing the window while connecting waits for it.

## Gotchas / decisions (do not undo without a reason)

- **ImPlot is pinned to v0.16.** ImPlot `master` has a new, incompatible `ImPlotSpec` API (`SetNextMarkerStyle` removed).
  ImGui is the **docking** branch tag v1.91.9 (multi-viewport lets Camera Image become its own OS window).
- ImGui default font has no Hangul -> **all UI strings are English**. README/docs are Korean.
- `io.IniFilename = nullptr`: layout is rebuilt by `DockBuilder` on every start. The bottom dock shows "Raw Data" because
  `drawRaw()` is submitted after `drawLog()` (setting `SelectedTabId` alone did not work).
- Camera image: Y axis is inverted (row 0 on top) so `PlotImage` needs uv flipped `(0,1)-(1,0)`; verified with an "L" marker in the demo image.
- `FpsCounter` uses atomics (worker writes, GUI reads). Control-call methods (`connect/start*/stop*/apply*`) are meant to be called from one thread.
- Config (`setConfig`) may only change while nothing runs; the UI disables the inputs while connected / streaming.
- The SDK EULA allows copy/distribute with notice retained, hence the vendored SDK. Keep `third_party/OXApi/LICENSE AGREEMENT_OX-SDK.pdf`.
- Original sources (not in the repo): SDK zip `OxSDK_V2_0_4.zip` and `Baumer_OX200-Firmware1.7_EN_V7_MNL.pdf`
  (www.baumer.com/OX200). UDP body layout: manual ch. 5.6 (pp. 43-46); SDK function list: ch. 5.7.

## Next steps (suggested order)

1. Run on the real sensor (checklist above), fix what breaks, then update the "Status" section.
2. Decide the invalid-point handling and the timestamp semantics from real data.
3. Integration into the target controller: link `baumer_sensor` (`add_subdirectory` or install + `libOXApi.so`), consume
   `ProfileFrame` (x/z in the sensor frame, mm). No calibration / hand-eye transform exists yet - sensor-to-TCP calibration is
   the next functional piece for robot use (weld seam tracking etc.).
4. Optional: record/replay of profiles (CSV is saved from the viewer today), software trigger / exposure / laser power
   controls (SDK has `Trigger`, `ConfigureExposureTime`, `ConfigureLaserPower`; none are wired into the UI yet).

## Conventions

C++20, `-Wall -Wextra` clean, no exceptions across the library API (errors returned via `std::string& err` and the log).
Commit messages in English, imperative. Keep `README.md` (Korean, user-facing) and this file (developer context) in sync.
