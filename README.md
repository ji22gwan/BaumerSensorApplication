# BaumerSensorApplication

Baumer **OXP200-R10C.005** (OX200 계열 2D 프로파일 센서) 용 standalone 테스트 앱 + 다른 제어기에 넣을 수 있는 라이브러리(`baumer_sensor`).

- Viewer: 프로파일 raw 데이터(X-Z, 강도, 수치 표/CSV) + 센서 카메라 이미지(별도 창)
- Baumer OX-Line SDK V2.0.4 (`libOXApi.so`) 포함 — 별도 설치 불필요 (Boost 의존성 없음)
- C++20, CMake, Ubuntu 20.04+ (amd64 / arm64 / armhf / i386 용 `libOXApi.so` 포함)

## 구성

```
lib/                 baumer_sensor — 제어기에 넣을 라이브러리 (SDK 헤더가 공개 헤더에 노출되지 않음)
  include/BaumerSensor/{BaumerSensor.h, SensorSource.h, Types.h}
  src/BaumerSensor.cpp
app/                 BaumerSensorApplication — Viewer (Dear ImGui + ImPlot + GLFW), --demo 용 DemoSensor
tools/               ox_udp_sim — 센서 없이 UDP 프로파일 스트림을 흉내내는 송신기
tests/               udp_loopback_test — 시뮬레이터 → 실제 libOXApi 파서 → BaumerSensor 자동 테스트
third_party/OXApi    Baumer SDK (헤더 + libOXApi.so + EULA)
third_party/imgui    Dear ImGui v1.91.9 (docking)      third_party/implot   ImPlot v0.16
```

## 빌드

```bash
sudo apt install build-essential cmake libglfw3-dev libgl1-mesa-dev
cmake -S . -B build
cmake --build build -j
ctest --test-dir build          # UDP 수신 경로 테스트 (센서 불필요)
```

## 실행

### 1) 센서가 있을 때 (제어 연결 + 폴링, 가장 간단)

```bash
./build/BaumerSensorApplication --host 192.168.0.250 --autostart
```
또는 인자 없이 실행해서 Connection 창에서 `Connect` → `Start profile` → `Start image`.

- PC 네트워크를 센서와 같은 대역(예: 192.168.0.x)으로 맞춰야 합니다.
- **센서 웹 인터페이스가 열려 있으면 `Connect`가 실패합니다** (SDK 제약: 설정 연결은 동시에 1개).
- `Poll` 방식은 제어 연결의 요청/응답이라 UDP보다 느립니다(슬라이더 상한 50 Hz, 실제 속도는 센서 응답 시간에 따라 다름 — 미측정). 고속은 UDP 사용.

### 2) UDP 스트림 (고속, 제어 연결 불필요)

센서가 이 PC로 UDP를 보내도록 설정해야 합니다. 둘 중 하나:
- 센서 웹 인터페이스의 UDP 설정에서 이 PC IP / 포트(12345) 지정 + 스트림 선택(Z-I profile 권장), 또는
- Viewer에서 `Connect` 후 `UDP target (sensor setup)` → `This PC IP` 확인 → 스트림 체크 → `Apply to sensor` (**센서 설정을 씀**)

```bash
./build/BaumerSensorApplication --udp --port 12345     # 제어 연결 없이 수신만
```
연결 후 UDP 스트림과 카메라 이미지를 동시에 쓸 수도 있습니다.

### 3) 센서 없이 확인

```bash
./build/BaumerSensorApplication --demo --autostart                  # 합성 데이터 (프로파일 + 이미지)
./build/ox_udp_sim --hz 100 &  ./build/BaumerSensorApplication --udp   # 가짜 UDP 스트림 → 실제 수신 코드
```

## Viewer 화면

| 창 | 내용 |
|---|---|
| Connection | IP/포트/precision, Connect, 프로파일 소스(Poll/UDP), 이미지, 통계(fps, 패킷 에러) |
| Profile (X-Z) | 프로파일 raw 점/선 [mm]. Auto-fit, Flip Z, Equal aspect, Hide Z=0 |
| Intensity | 점별 반사 강도(raw) |
| Camera Image | 센서 카메라 raw 8bit 이미지(ROI). False color, 포화(255) 표시, 줌/팬, `Save PGM` |
| Raw Data | 최신 프로파일 점 표 + `Save profile CSV` |
| Log | 연결/스트림/에러 메시지 |

창은 도킹 가능하고, **메인 창 밖으로 끌어내면 별도 OS 창**이 됩니다(예: Camera Image를 두 번째 모니터로).

## 라이브러리로 쓰기 (다른 제어기)

```cmake
add_subdirectory(BaumerSensorApplication)     # 또는 lib/ 와 third_party/OXApi 만 복사
target_link_libraries(my_controller PRIVATE baumer_sensor)
```
```cpp
#include <BaumerSensor/BaumerSensor.h>

baumer::BaumerSensor s;
s.setConfig({.host = "192.168.0.250", .udpPort = 12345, .precision = 100.0});
std::string err;

// (a) UDP 스트림
s.startUdp(err);
// (b) 또는 제어 연결 + 폴링 (precision 을 센서에서 읽어옴) + 이미지
s.connect(err);  s.startPolling(10.0, err);  s.startImage(2.0, err);

baumer::ProfileFrame f;  std::uint64_t seq = 0;
while (running) {
    if (s.latestProfile(f, seq)) { /* f.x[i], f.z[i] [mm], f.intensity[i] */ }
}
```
- `latest*()` 는 새 프레임이 있을 때만 복사해서 `true` 반환 (워커 스레드가 최신 프레임을 보관).
- 공개 헤더는 Baumer SDK 헤더를 include 하지 않습니다. 링크 시 `libOXApi.so`가 필요하고(`$<LINK_ONLY:OXApi>`로 자동 전파), 실행 시 RPATH로 찾습니다. 설치: `cmake --install build --prefix <dir>` → `bin/`, `lib/libOXApi.so`.
- 제어 호출(`connect`, `start*`, `stop*`, `applyUdpStreams`)은 한 스레드에서 호출하세요.

## 값/단위

- x, z [mm] = raw / `precision`. 폴링은 SDK 문서대로 `x = (X + XStart)/Precision`, UDP는 `x = X/Precision`.
- `precision`은 `connect()` 때 센서에서 읽습니다. UDP만 쓰면 기본 100 (`--precision`으로 변경).
- `timestamp`는 SDK가 주는 raw 값 그대로, fps는 PC 도착 시각 기준입니다.
- 이 라이브러리는 3D 변환/캘리브레이션을 하지 않고 센서 프레임(X-Z) 그대로 제공합니다.

## 검증 상태 (중요)

| 항목 | 상태 |
|---|---|
| UDP 수신 (단일/분할 패킷, Z / I / Z-I, 값 변환) | 시뮬레이터 + 실제 `libOXApi.so` 파서로 자동 테스트 통과 |
| Viewer GUI (프로파일, 강도, 이미지 방향, Raw 표, 연결 실패 처리) | `--demo` / 시뮬레이터로 확인 |
| **센서 실물**: `Connect`, 폴링(`GetIntensityProfile`), 카메라 이미지(`GetImage`), `Apply to sensor` | **아직 실기 미검증** (SDK 호출 코드는 Baumer 샘플 기준으로 작성) |
| Z 무효값 표기 | 매뉴얼에 없음 → 그대로 표시. 무효가 0으로 오면 `Hide Z=0` 사용 |

실기에서 문제가 생기면 Log 창의 메시지(SDK 예외 문구 포함)를 알려주세요.

## 라이선스 / 출처

- `third_party/OXApi`: Baumer OX-SDK V2.0.4 (LICENSE AGREEMENT 참조, 저작권 고지 유지 조건으로 복사·배포 허용). 원본: www.baumer.com/OX200
- Dear ImGui (MIT), ImPlot (MIT): 각 폴더의 LICENSE
- 패킷 포맷 참고: OX200 Operating Manual, Firmware 1.7, 5장 Interfaces and protocols
