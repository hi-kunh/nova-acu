# 작업 목록 (TODO)

ACU 프로젝트의 진행 상황과 남은 일 정리.
완료된 단계의 **상세 기록은 [README.md](README.md)** 에 있고, 이 파일은 "무엇이 남았는지"만 관리한다.

- 최종 갱신: 2026-09-08
- 현재 위치: **5단계 완료 / 5.5단계(ACU 단독 개통) 진행 중** — RRU 개발보드가 오기 전까지
  **CM3 IO 보드에 acud를 올리고 PC(상위 시스템)와 TCP 통신을 개통**하는 것이 당면 작업.
  6단계(RRU 연동)는 하드웨어 확정 완료, 개발보드 구매 대기

---

## 진행 상황 요약

| 단계 | 내용 | 상태 |
|------|------|------|
| 0 | 데몬 뼈대 (시그널 처리, 메인 루프) | ✅ 완료 |
| 1 | SQLite3 + 출입 판정 로직 | ✅ 완료 |
| 2 | 하드웨어 추상화 계층(HAL) 인터페이스 확정 | ✅ 완료 |
| 3 | config.json 감시 + 무중단 리로드(SIGHUP) | ✅ 완료 |
| 4 | 웹 설정 인터페이스 (Flask) | 🔶 1차 범위만 (config 편집) |
| 5 | 네트워크 통신부 (TCP, IDTi 프로토콜 V2) | 🔶 1차 범위만 (Event Log 응답) |
| 5.5 | **ACU 단독 개통 (CM3 보드 + PC 통신)** | 🔶 진행 중 (아래 전용 절) |
| 6 | 실제 하드웨어 (USB, ACU↔RRU) | ⬜ 개발보드 대기 |
| 7+ | 배포/운영 (systemd, 보안 강화 등) | ⬜ 미착수 |

범례: ✅ 완료 · 🔶 부분 완료(남은 항목 아래 참고) · ⬜ 미착수

---

## 진행 중: 5.5단계 — ACU 단독 개통 (CM3 IO 보드 + PC 통신)

RRU 개발보드가 도착하기 전까지 할 수 있는 일. **acud를 실제 CM3 IO 보드에서 돌리고, PC(상위 시스템)와
TCP로 통신을 개통**하는 것이 목표. 리더/릴레이는 mock으로 대체한다.

**확정된 전제 (2026-09-08)**

- 연결 방향: **PC가 TCP 클라이언트로 ACU에 접속해 온다.** ACU는 지금처럼 listen 하는 서버가 맞음
  (근거: `Platinum/Source/IntelliScan NET Platinum/clsAsynchronousClient.cs` — `BeginConnect`로 장치에 접속)
- 상대 PC 프로그램: **IntelliScan NET Platinum** (기존 상위 시스템). 기본 포트 **1004**
  (`clsAsynchronousClient.cs:35 defaultPortNumber`). 우리 기본값은 9870이므로 맞춰야 함
- PC 프로그램 소스 위치: `/home/jayden/workspace/idti/` — **`DeveiceManager/`(DM)에 실제 통신 코드가 있다.**
  `DeveiceManager/Source/isldev/`가 프레임·이벤트·상태 구조 구현체(`clsDevFrame.cs`, `clsDevCommand.cs`,
  `clsDevEvent.cs`, `clsDevStatus.cs`), `Source/IntelliScan Device Manager/`가 그걸 쓰는 응용.
  소스 주석은 CP949 -> `iconv -f CP949 -t UTF-8` 로 볼 것.
  **문서에서 애매한 것은 이 소스를 근거로 삼는다.** 확인한 내용은 README "PC 소스에서 확인한 프로토콜 사실" 참고

### 5.5-1. 통신 안정성 (실제 PC를 붙이기 전 선행) — ✅ 완료 2026-09-08

- [x] **SIGPIPE로 데몬이 죽는 문제** — `main.c`에서 SIGPIPE 무시 + `net.c`의 send에 `MSG_NOSIGNAL`.
      수정 전 바이너리로 재현됨(응답을 안 읽고 끊는 클라이언트 3회 만에 프로세스 종료), 수정 후 생존 확인
- [x] **부분 전송 처리** — `AcuNet`에 송신 버퍼(4KB) 추가. `send()`가 일부만 보내면 나머지를 남겨 두고
      `select()`의 writefds로 이어 보냄. 전송 실패 시 이벤트를 큐에서 빼지 않아 유실되지 않게 함
- [x] **mock HAL 카드 주입식으로 변경** — `acud_mock.fifo`로 카드/센서를 주입. 예전의 2초마다 자동 태그는
      `auto on`으로만 동작. (이전에는 이벤트 큐 32개가 1분이면 가득 차 통신 테스트가 불가능했음)
- [x] **테스트 클라이언트 저장소에 포함** — `tools/idti_client.py` (PC 역할, 접속하는 쪽)

### 5.5-2. 보드에 올리기 (다음 작업)

- [ ] **OS 이미지 기록(flash) 및 첫 부팅** — 이미지는 결정됨, 아래 "5.5-4" 참고
      (`xz -dk` 로 풀고 SD에 기록 -> 부팅 -> SSH 접속)
- [ ] 빌드 방식 결정 — 보드에서 네이티브 빌드(권장) vs 개발 PC 크로스 컴파일
- [ ] 보드에 의존성 설치 (`build-essential libsqlite3-dev libcjson-dev python3-venv`)
- [ ] **보드 네트워크** — 고정 IP(또는 DHCP 예약), PC에서 포트 도달 확인
- [ ] **시간 동기화** — NTP 또는 RTC. IDTi 이벤트는 BCD 시각을 싣기 때문에 시각이 틀리면 이벤트가 무의미.
      타임존(KST) 설정도 함께
- [ ] **경로 절대화** — 현재 `config.json`/`acud.db`/`acud.pid`/`acud_mock.fifo`를 전부 cwd 상대경로로 씀.
      데몬으로 띄우려면 절대경로 또는 WorkingDirectory 지정 필요
- [ ] **systemd 유닛** (`acud.service`) + 부팅 시 자동 시작
  - [ ] **포트 1004는 특권 포트(<1024)다** — 일반 사용자로 띄우면 bind가 실패한다.
        `AmbientCapabilities=CAP_NET_BIND_SERVICE`를 주거나 root로 실행할 것.
        (현재 `net_init` 실패 시 데몬은 계속 도니 조용히 통신만 안 되는 상태가 된다 — 로그 확인 필요)
- [ ] **로그를 stdout -> journald/파일**로 (지금 stdout만이라 데몬화하면 로그가 사라짐)
- [ ] 웹UI를 보드에서 띄울지 결정 (`webui/app.py:178`이 `0.0.0.0`, Flask 개발 서버)

### 5.5-3. PC와 실제 통신 개통

- [ ] **포트를 1004로 맞출지 결정** — DM/Platinum 기본값이 1004. config.json의 `tcp_port` 변경 또는 PC 쪽 설정
- [x] ~~접속 직후 첫 명령에 응답하기~~ -> **구현 완료 (2026-09-08)**.
      `RequestStatus(0x04)/Read(0x02)/Firmware(0x2A)` 요청에 `SendStatus(0x03)` + Firmware Info 268byte로 응답.
      상세는 README "Firmware(0x2A) 상태 요청 응답 구현" 참고
  - [ ] **응답 Command가 `SendStatus(0x03)`가 맞는지 PC와 붙여 확인** — Command Table의 Send/Request 짝으로
        추정한 값이다. PC 소스에서 응답 검증 코드를 못 찾음(SDK DLL 안으로 보임)
  - [ ] **Firmware Version / 빌드일시 값 확정** — 현재 1.0.0.0 / 2026-09-08 (`protocol.h`의 `IDTI_FW_*`)
- [x] ~~`IsExcludeDeviceStatus` 비트 처리~~ -> **구현 완료**. 켜지면 Device Status를 빼고, 그 비트를
      응답 Frame Option에도 실어 PC가 응답 구성을 알 수 있게 함
- [ ] **이벤트 수집 모델을 인덱스 방식으로 재검토** — PC는 HistoryCount(5)로 개수를 묻고, HistoryIndex(6)로
      읽기 위치를 옮기고, History(1)로 받아가고, Init으로 리셋한다. 또 `IsReRequestEvent` 비트로 직전 이벤트를
      다시 요청할 수 있다. **현재 우리 구현은 전송 즉시 큐에서 빼므로 재요청을 만족시킬 수 없다**
- [ ] **`IsTimeSync` 비트 처리** — Event Request에 실려 오는 시각 동기화 요청
- [x] ~~Device Status 첫 바이트는 Category, 둘째가 DeviceType~~ -> 상수를 `IDTI_DEVICE_CATEGORY` +
      `IDTI_DEVICE_TYPE`으로 분리함 (나가는 바이트는 0x00, 0x29로 동일)
  - [ ] **Category 0이 유효한 값인지 확인** — PC가 어떻게 해석하는지 미확인
- [ ] **ModuleIOStatus 니블 인코딩** — 14byte 각각 상위 니블=IO Type, 하위 니블=IO Status.
      6단계에서 RRU를 IO 모듈로 보고할 때 이 형식을 따라야 함
- [ ] **패킷 검증 보강** — Protocol Version, Tail의 ETX 미검증 (현재 STX/헤더 체크섬만 확인)
- [ ] **Password(헤더 4byte) 검증** — 지금은 파싱만 하고 검증하지 않음.
      PC는 컨트롤러별 설정으로 `IsPassword` 비트(Frame Option `[4]` bit6)를 켤 때만 의미가 있게 쓴다
- [ ] **수신 버퍼 512byte 고정** — 이보다 큰 요청은 통째로 버림. 유저 DB 전송 받으려면 동적 버퍼 필요
- [ ] Time Sync 처리
- [ ] 한 응답에 이벤트 여러 건 싣기 (현재 1건씩)
- [ ] 장치 타입 코드 확정 (현재 `0x29` 임시값) — PC가 어떤 타입을 기대하는지 확인
- [ ] 이벤트 큐 영속화 (현재 32개 메모리 링버퍼, 재시작 시 유실)

### 5.5-4. CM3 OS — **결정됨: Radxa 공식 Debian Bullseye (XFCE) b25**

```
/home/jayden/workspace/radxa/cm3/radxa-cm3-io_debian_bullseye_xfce_b25.img.xz   (1.1GB, xz 압축)
```

Radxa가 CM3 IO 보드용으로 낸 공식 이미지라 이더넷/eMMC/USB가 검증돼 있다. `apt`로 의존성을 바로 깔 수 있어
보드에서 네이티브 빌드가 가능하고, 파이썬이 들어 있어 웹UI(Flask)도 그대로 돌아간다.

- [ ] **이미지 기록** — 위 파일명/빌드 번호(b25)를 그대로 남길 것. 재현 가능해야 함
- [ ] **부팅 후 바로 확인할 것**
  - [ ] `uname -r` — 커널 버전 (Rockchip BSP 계열인지)
  - [ ] `modinfo cdc_acm` 또는 `lsmod` — **6단계에서 RRU를 `/dev/ttyACM0`으로 붙이려면 필수**
  - [ ] `cat /etc/debian_version`, `dpkg --print-architecture` (arm64여야 함)
  - [ ] 기본 계정/비밀번호 (Radxa 이미지는 통상 `rock`/`rock`) -> **즉시 변경**
- [ ] **apt 저장소 동작 확인 (주의)** — Bullseye(Debian 11)는 **2026년 8월로 LTS가 끝났다.**
      미러가 `archive.debian.org`로 옮겨져 `apt update`가 실패할 수 있다. 실패하면 `sources.list`를
      archive로 바꿔야 한다. **보드 받자마자 제일 먼저 확인할 것**
- [ ] **의존성 설치** — `build-essential libsqlite3-dev libcjson-dev python3-venv`
      (bullseye의 pip은 오래돼서 Flask 3.x 설치 전에 `pip install -U pip` 필요할 수 있음)
- [ ] **XFCE 데스크톱 끄기** — 우리 제품은 headless다. `systemctl set-default multi-user.target`으로
      디스플레이 매니저를 내리면 메모리/부팅시간이 준다 (개발 중에는 켜 두고 써도 무방)
- [ ] **설치 매체 결정** — SD 부팅으로 먼저 개통하고, 제품은 eMMC로 갈지.
      eMMC 기록은 USB-C OTG + maskrom(rkdeveloptool) 경로가 필요하다
- [ ] 제품화 단계는 별도 결정 — read-only rootfs + overlayfs로 굳히기, 또는 Yocto/Buildroot로 축소.
      Bullseye가 이미 oldstable이라 **제품 출하 시점에는 배포판을 다시 정해야 할 가능성이 높다.**
      지금 코드가 배포판 고유 기능에 의존하지 않게만 해 두면 나중에 갈아탈 수 있다

---

## 대기 중: 6단계 — RRU 연동 (USB)

> 개발보드 도착 후 착수. 그전까지는 위 5.5단계를 진행한다.

하드웨어 구성이 확정되면서 6단계 내용이 바뀌었다. **RK3566에서 Wiegand를 직접 받는 것이 아니라,
별도 RRU 보드(리더8 / 입력24 / 출력8)와 USB로 통신**한다. 구성 상세는 [HARDWARE.md](HARDWARE.md).

### 6-0. 설계 확정 (코드 작성 전)

> **다음 작업 시작 지점**: NUCLEO-C562RE 개발보드 구매 (아래 첫 항목).
> 보드가 오면 **8모듈 중 2모듈만 구현**해 프로토콜을 검증한다.

- [x] ~~Wiegand 수신용 외부 인터럽트 핀 확인~~ -> **STM32C562 외부 인터럽트로 충분함 확인.
      D0/D1 16개 직결(A안) 채택**. 게이트로 묶는 B안은 폐기. (HARDWARE.md "주의 3")
  - 남은 것은 개수가 아니라 **최종 보드의 16핀 배치**(인터럽트 라인 공유 회피) — 실보드 설계 단계 사안
- [ ] **[최우선] NUCLEO-C562RE 개발보드 구매**
- [ ] **개발보드 도착 후 2모듈 구성으로 검증** — 개발보드는 8모듈분 I/O가 없으므로,
      본 제품(8모듈) 보드를 설계하기 **전에** 2모듈로 프로토콜을 먼저 검증한다.
      모듈 1개 = 리더1 + 입력3(Exit/Door/Lock) + 출력1 = 6핀 -> 2모듈 = 12핀.
      검증 범위/한계는 HARDWARE.md "개발 단계 계획" 절 참고
  - [ ] 보드 브링업 — USB CDC-ACM 열거 확인(`/dev/ttyACM0`), ST-LINK VCP로 디버그 로그 분리
  - [ ] 리더 2대 + 입력 6 + 출력 2 배선 (출력은 릴레이 모듈 또는 LED 대체 가능)
  - [ ] Wiegand 수신 펌웨어 (인터럽트 직결, 26/34bit 프레임 조립)

- [x] ~~RRU 대수~~ → **1대 고정 (point-to-point)**, ~~설치 형태~~ → **같은 함체 내부** (확정)
  - 결과: 프레임에 **장치 주소 필드 불필요**, 카드 이벤트는 **RRU 능동 송신**
  - 물리 계층이 USB든 UART든 이 두 결론은 그대로 유지됨
- [x] ~~baud/프레이밍~~ → **115200 8N1** (확정)
- [x] ~~물리 계층~~ → **USB 확정** (IO 보드 J18 = 2.54mm 10핀 내부 USB 헤더 사용).
      UART의 핀먹스/콘솔 충돌 문제가 전부 사라짐. UART 조사 기록은 HARDWARE.md에 참고용으로 보존
- [x] ~~RRU MCU 선정~~ → **STM32C562 최종 확정**. 개발은 NUCLEO-C562RE 보드에서 시작
  - 보드에 네이티브 USB Type-C Device 커넥터 내장 → CDC-ACM, `/dev/ttyACM0`. 브릿지 IC 불필요
  - ST-LINK VCP가 별도 채널로 남아 RRU 디버그 로그를 프로토콜 링크와 분리 가능
  - CAN FD 커넥터(CN18)도 있어 나중 RK3568+CAN 경로를 같은 보드에서 검증 가능
  - 주의: USB-C로 보드 급전 불가(self-powered 전제) → VBUS/GND 처리 정할 것
- [ ] **최종 RRU 보드 I/O 설계 (2모듈 검증 완료 후)** — LQFP64(`RE`)로는 48 I/O(Wiegand16+입력24+출력8)가
      한계 초과. MCU는 STM32C562로 고정하되 **패키지는 재검토**: 계열 내 상위 핀수 옵션 확인,
      느린 입력24/출력8은 I/O 익스팬더로 분리(단, Wiegand D0/D1에는 익스팬더 금지),
      16개 Wiegand 핀 배치 확정
- [ ] J18(내부 USB 헤더) 결선 정의 — 2포트 중 사용 포트, VBUS 연결 여부, GND 처리
      (RRU는 자체 전원 사용. 리더8+릴레이8은 USB 500mA로 감당 불가이고,
       NUCLEO-C562RE는 USB-C로 급전도 안 됨)
- [ ] udev 규칙으로 `/dev/rru` 고정 심볼릭 링크 (STM32 CDC-ACM의 VID/PID 기준)
- [ ] **ACU↔RRU 프로토콜 설계** — 프레임 구조, CRC, 릴레이 제어 명령, 센서 상태 조회/통지,
      타임아웃·재전송, 연결 끊김 감지
  - **USB 재열거 대비 필수**: RRU가 이벤트를 자체 버퍼에 쌓고 ACU가 ACK할 때까지 유지
    (링크가 1~2초 끊겨도 카드 태그가 유실되면 안 됨). 릴레이 EMI로 재열거가 실제로 일어날 수 있음
- [ ] Wiegand 카드 ID 형식(26/34bit) → DB `card_id`(hex 16자) 매핑 규칙 확정
- [ ] 채널 번호 체계를 RRU 펌웨어와 일치시키기 (HARDWARE.md 표는 제안값)

### 6-1. 기존 코드의 단일 도어 전제 걷어내기

1~5단계는 **단일 도어/단일 리더**를 전제로 만들어져 있어 8도어 구성과 어긋난다.
**소프트웨어는 처음부터 8채널 전제로 고친다** — 개발보드에서 2모듈만 쓰는 것은 하드웨어 검증 범위의
제약일 뿐이다. 채널 수는 config로 두고 개발 단계에서만 2로 쓴다.

- [ ] **`hal.h` 인터페이스에 채널 차원 추가** — `hal_read_card()`/`hal_open_door()`/`hal_read_sensor()`
      모두 "어느 리더/도어인지" 인자가 없음. 2단계에 "main.c/access.c는 안 건드린다"고 적었지만
      이 부분은 예외로 수정 불가피
- [ ] **`net.c:212` Reader Address 하드코딩 해제** — 현재 `0x00` 고정("단일 리더 → 0"), 실제 리더 번호로 교체
- [ ] **DB 스키마에 도어별 출입 권한 추가** — 현재 `cards`는 카드의 허용/거부만 판정할 뿐
      "8개 도어 중 어디를 열 수 있는가"를 표현 못 함. IDTi Group / User General Group 개념
      (미확인 문서 `10.`, `11.`) 확인 후 스키마 확장
- [ ] **`main.c` 루프를 8리더 처리로 확장** — 현재는 카드 1건씩 순차 판정
- [ ] `CARD_POLL_INTERVAL_MS`(현재 2000ms) 재검토 — RRU가 카드 이벤트를 능동 송신하므로 카드 폴링
      자체가 없어진다. 메인 루프를 "UART 수신 + TCP 처리"를 함께 기다리는 구조로 바꿀 것
      (5단계에서 만든 카드주기/네트워크 분리 로직도 이때 같이 정리)
- [ ] `hal_open_door()` 논블로킹화 — 현재 mock은 로그만 찍어 드러나지 않지만, 실제로 N초 블로킹하면
      그동안 TCP 응답과 카드 조회가 멈춤 (만료 시각을 메인 루프에서 확인하는 방식 검토)
- [ ] Device Status `ExistedModule`/`IOModuleStatus` 재검토 — 현재 전부 0. RRU가 IDTi Remote Module
      위치에 해당하므로 보고 필요한지 확인 (문서 `8. System Device Reader Setup`)

### 6-2. 구현

- [ ] `acud/rru.c` / `rru.h` — 시리얼 노드 열기/닫기, 프레임 송수신, CRC, 재전송 (전송 계층을 분리해
      나중에 RK3568 CAN으로 갈아끼울 수 있게)
  - [ ] 장치 없을 때/끊겼을 때 재연결 로직 (부팅 시 USB 열거가 늦을 수 있음)
- [ ] `acud/hal_rru.c` — 위 프로토콜을 `hal.h` 인터페이스로 감싼 실제 구현체
- [ ] Makefile HAL 구현체 선택 스위치 (`HAL=mock|rru`)
- [ ] config.json에 RRU 링크 설정 추가 (장치 경로 `/dev/rru`, baud 115200) + 웹 UI 반영
- [ ] 출력 채널 NO/NC 설정 항목 노출

### 6-3. 검증

- [ ] RRU 없이 동작 확인 — 장치 노드가 없어도 데몬이 죽지 않을 것(기존 net_init fail-safe와 동일 원칙)
- [ ] USB 케이블을 뽑았다 꽂아 재연결되는지 + 그 사이 태그된 카드가 유실되지 않는지
- [ ] 실제 카드 태그 → 판정 → 해당 도어 릴레이 동작 엔드투엔드
- [ ] 리더 각각 구분되어 판정되고, 이벤트의 Reader Address가 올바른지
      (개발보드 단계에서는 **2채널로 검증** — 다채널 구조 자체는 2채널로도 드러난다. 8채널은 실보드)
- [ ] Exit 버튼 입력 → 판정 없이 릴레이 동작
- [ ] Door/Lock 센서 상태가 이벤트 Door Status에 반영되는지
- [ ] README에 6단계 완료 기록 추가

---

## 단계별 남은 항목 (부분 완료분)

### 1단계 관련
- [ ] **공휴일 캘린더(Device Holiday) 미구현** — `timezones.week_select` 의 공휴일 비트(bit8~0)가 반영되지 않음. 현재는 요일 비트만 확인
- [ ] 카드 등록/삭제 경로가 없음 (지금은 DB를 직접 조작해야 함) → 4단계 카드 CRUD 또는 5단계 유저 DB 전송으로 해결

### 4단계 (웹 설정 인터페이스) 남은 것
- [ ] **카드 관리(CRUD) 화면** — 카드 등록/수정/삭제, 유효기간·시간대 그룹 지정
- [ ] `tcp_port` 편집 필드 추가 (현재는 값만 보존하고 화면에는 없음)
- [ ] 이벤트 로그 조회 화면
- [ ] **보안 강화 (배포 전 필수)**
  - [ ] 로그인 실패 횟수 제한 / 잠금 (현재는 1초 지연만 있음)
  - [ ] 4자리 숫자 PIN → 더 강한 인증 수단 검토
  - [ ] HTTPS 적용
  - [ ] CSRF 방어
  - [ ] 사내망/로컬 한정 바인딩 (현재 개발 서버는 전 인터페이스 노출 가능)
  - [ ] Flask 개발 서버 → 운영 WSGI 서버(gunicorn 등)로 전환

### 5단계 (네트워크 통신부) 남은 것

> 이 중 PC 개통에 직접 걸리는 것(Device Status 응답, Time Sync, 다중 이벤트, 큐 영속화, 장치 타입 코드)은
> 위 **5.5-3**에서 관리한다. 여기는 그 외 남은 것.

- [ ] **유저 DB 대량 송수신** (UserBinaryTransmit, Object `0xD0`/`0xD1`) — 상위 시스템에서 카드 내려받기
- [ ] Device Output 원격 제어 (원격 문 열기)
- [ ] Time Sync 처리
- [ ] Device Timezone / Validation / Holiday 원격 설정 수신
- [ ] 한 응답에 이벤트 여러 건 싣기 (현재는 1건씩만, Data Block Index로 분할 전송)
- [ ] 이벤트 큐 영속화 — 현재 32개 메모리 링버퍼라 재시작 시 유실, 가득 차면 오래된 것부터 버림
- [ ] 동시 다중 접속 지원 여부 결정 (현재 1개 연결만)
- [ ] 장치 타입 코드 확정 — 현재 `HAL_DEVICE_TYPE_ISC101 0x29` 임시값

### 7단계 이후 (배포/운영)
- [ ] systemd 유닛 파일 (`acud.service`, `acud-webui.service`) + 부팅 시 자동 시작
- [ ] 워치독 / 비정상 종료 시 자동 재시작
- [ ] 로그 파일 출력 + 로테이션 (현재는 stdout만)
- [ ] 타깃 보드용 크로스 컴파일 / 패키징 방법 정리
- [ ] 자동화 테스트 (현재는 수동 검증만)

---

## 기술 부채 / 확인 필요

- [ ] `protocol.c` 유닛 테스트 없음 — 소켓과 분리해 뒀으니 테스트 붙이기 좋음
- [ ] 참고 문서 중 아직 안 읽은 것: `3. Member(User DB) Structure`, `6. FileBinaryTransmit`,
      `8. System Device Reader Setup`, `10. Group`, `11. User General Group`, `13. Force OpenMode`,
      `14. Request Blocking User List` — 필요한 단계에서 확인
- [ ] 루트의 `index.html` — 초기 커밋 잔재. 용도 확인 후 정리하거나 삭제

---

## 다른 PC에서 작업 이어가기

원격 저장소: `git@github.com:hi-kunh/nova-acu.git`

### 1. 최초 1회 세팅

```bash
# SSH 키가 없다면 생성 후 GitHub에 공개키 등록
ssh-keygen -t ed25519 -C "ghsmile@naver.com"
cat ~/.ssh/id_ed25519.pub     # → GitHub Settings > SSH and GPG keys 에 등록
ssh -T git@github.com         # 연결 확인

git clone git@github.com:hi-kunh/nova-acu.git
cd nova-acu
```

### 2. 빌드 의존성 설치 (Debian/Ubuntu 기준)

```bash
sudo apt-get install build-essential libsqlite3-dev libcjson-dev python3-venv
```

### 3. 빌드 & 실행

```bash
cd acud && make run           # 데몬

cd ../webui                   # 웹 설정 화면 (별도 터미널)
python3 -m venv .venv
./.venv/bin/pip install -r requirements.txt
./.venv/bin/python app.py     # http://localhost:5000
```

### 4. 작업 흐름

```bash
git pull            # 작업 시작 전 항상 최신화
# ... 작업 ...
git add -A && git commit -m "..."
git push            # 그날 작업이 끝나면 push
```

### 주의: 커밋되지 않는 것들 (PC마다 새로 만들어짐)

`.gitignore` 대상이라 다른 PC로 따라가지 않는다:

- `acud/acud` (빌드 산출물) → `make` 로 다시 빌드
- `acud/acud.db` (카드/유효기간/시간대 DB) → 첫 실행 시 빈 스키마로 새로 생성됨.
  **테스트 카드 데이터는 PC 간에 공유되지 않으므로 각자 다시 넣어야 함**
- `acud/acud.pid` → 실행 시 자동 생성
- `webui/.venv/` → 위 3번대로 다시 생성

`acud/config.json` 은 **커밋 대상**이라 따라간다. 단 `admin_password` 가 기본값 `"0000"` 그대로이므로
실제 배포 시에는 반드시 변경할 것 (변경하면 커밋에 비밀번호가 남으므로, 배포 단계에서는
config.json 을 gitignore 로 돌리고 예시 파일만 커밋하는 방식으로 바꿀 것 — 위 4단계 보안 항목 참고).

### 참고 문서 경로 문제

IDTi 프로토콜 문서는 이 저장소가 아니라 로컬 경로
`/home/jayden/workspace/idti/IDTI WebApp Protocol/` 에 있다. 다른 PC에서 문서를 봐야 한다면
그 폴더를 따로 복사해 가야 한다 (저장소에 포함되어 있지 않음).
