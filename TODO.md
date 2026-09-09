# 작업 목록 (TODO)

ACU 프로젝트의 진행 상황과 남은 일 정리.
완료된 단계의 **상세 기록은 [README.md](README.md)** 에 있고, 이 파일은 "무엇이 남았는지"만 관리한다.

- 최종 갱신: 2026-09-09
- 현재 위치: **5단계 완료 / 5.5단계(ACU 단독 개통) 진행 중** — RRU 개발보드가 오기 전까지
  **CM3 IO 보드에 acud를 올리고 PC(상위 시스템)와 TCP 통신을 개통**하는 것이 당면 작업.
  6단계(RRU 연동)는 하드웨어 확정 완료, 개발보드 구매 대기
- **2026-09-09: 보드 개통 1차 성공.** acud가 CM3 IO 보드에서 돌고, 개발 PC와 TCP로 상태 요청 응답 +
  카드 이벤트 수신까지 확인했다. 상세는 [README.md](README.md) "보드 개통 성공 (2026-09-09)".
  **다음 시작 지점: 아래 "5.5-0. 다음 여기서 시작" 절.**

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
| 5.5 | **ACU 단독 개통 (CM3 보드 + PC 통신)** | 🔶 **개통 + 데몬화 완료 (2026-09-09)** / 실제 PC 연동 남음 |
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

### 5.5-0. 다음 여기서 시작 (2026-09-09 갱신)

**개통 1차 성공까지 끝났다.** 아래 1~5번은 모두 완료. 상세 기록은 README 참고.

**보드 접속**: `ssh rock@192.168.0.132` (wlan0. 개발 PC는 192.168.0.73). SSH 키 인증 설정됨

- [x] ~~1. 타임존 변경~~ -> **완료**. `Asia/Seoul` (KST, +0900), NTP 동기화 정상
- [x] ~~2. apt 고치기~~ -> **완료**. 단 **앞선 조치안대로는 실패한다** — `bullseye-security`도 함께
      비활성화해야 했다(풀의 .deb가 404). 아래 5.5-4의 갱신된 블록 참고
- [x] ~~3. 빌드 의존성 설치~~ -> **완료**. `libsqlite3-dev`, `libcjson-dev`, `libc6-dev`
      (헤더 2개면 된다는 판단은 틀렸다 — `libc6-dev`가 아예 없었다)
- [x] ~~4. 저장소 복사 후 빌드~~ -> **완료**. `git clone`은 보드에 GitHub 키가 없어 실패 -> **rsync로 확정**
      ```bash
      # 개발 PC에서
      rsync -av --exclude .git --exclude '__pycache__' ~/workspace/nova-acu/ rock@192.168.0.132:~/nova-acu/
      ```
      빌드는 경고 0개, `acud` ELF aarch64 PIE 39,520byte
- [x] ~~5. PC에서 접속 확인~~ -> **완료. 개통 1차 성공**.
      Firmware 상태 요청 548byte 응답, 카드 주입 -> 허용 판정 -> 이벤트 316byte 수신

**다음 할 일 (우선순위 순)**

- [ ] **빌드/배포 방식을 바이너리 전용으로 전환** — 아래 5.5-5 (새 절). 제품 ACU에 소스와
      툴체인이 남지 않게 하는 작업. 개통이 끝났으니 이제 착수할 시점이다
- [x] ~~**5.5-2** 경로 절대화, systemd 유닛, 로그를 journald로~~ -> **완료 2026-09-09**. eth0도 개통됨
- [ ] **5.5-3** 포트 1004 전환 후 실제 PC 프로그램(IntelliScan NET Platinum)과 통신
- [ ] 보드 기본 계정 비밀번호 변경 (`rock`/`rock` 그대로다)

### 5.5-5. 빌드/배포를 바이너리 전용으로 (2026-09-09 신설)

**배경**: 제품 ACU에는 소스코드도, 빌드 툴체인도 남아 있으면 안 된다. 지금은 개통을 우선하느라
보드에서 네이티브 빌드했지만 이건 개발 단계 한정이다.

**주의: 단순 크로스 컴파일은 그대로는 안 된다**

| | 개발 PC | 보드 |
|---|---------|------|
| OS | Ubuntu 26.04 | Debian 11 bullseye |
| glibc | **2.43** | **2.31** |

Ubuntu의 `gcc-aarch64-linux-gnu`로 빌드하면 glibc 2.43 심볼을 참조해 보드에서 실행이 안 된다
(`version 'GLIBC_2.3x' not found`). `-lsqlite3 -lcjson` 링크용 arm64 sysroot도 따로 필요하다.

- [ ] **bullseye arm64 컨테이너 빌드 환경** (podman/docker + qemu-user-static). 타깃과 glibc·라이브러리가
      정확히 일치하고, 그대로 CI/릴리스 절차가 된다. 개발 PC에는 podman/docker/qemu 모두 미설치 상태
- [ ] 배포를 **`scp acud` 하나**로 좁히기 (현재는 rsync로 소스 전체를 밀고 있음)
- [ ] `strip`으로 심볼 제거
- [ ] **webui는 크로스컴파일로 해결되지 않는다** — Flask `app.py`가 그 자체로 소스다.
      제품에 webui를 넣을지, 넣는다면 소스 노출을 어떻게 다룰지 별도 결정 필요
- [ ] 제품 이미지 절차 정의 (7단계와 연계): gcc/make/git/`*-dev` 제거, read-only rootfs, 소스 미포함

### 5.5-1. 통신 안정성 (실제 PC를 붙이기 전 선행) — ✅ 완료 2026-09-08

- [x] **SIGPIPE로 데몬이 죽는 문제** — `main.c`에서 SIGPIPE 무시 + `net.c`의 send에 `MSG_NOSIGNAL`.
      수정 전 바이너리로 재현됨(응답을 안 읽고 끊는 클라이언트 3회 만에 프로세스 종료), 수정 후 생존 확인
- [x] **부분 전송 처리** — `AcuNet`에 송신 버퍼(4KB) 추가. `send()`가 일부만 보내면 나머지를 남겨 두고
      `select()`의 writefds로 이어 보냄. 전송 실패 시 이벤트를 큐에서 빼지 않아 유실되지 않게 함
- [x] **mock HAL 카드 주입식으로 변경** — `acud_mock.fifo`로 카드/센서를 주입. 예전의 2초마다 자동 태그는
      `auto on`으로만 동작. (이전에는 이벤트 큐 32개가 1분이면 가득 차 통신 테스트가 불가능했음)
- [x] **테스트 클라이언트 저장소에 포함** — `tools/idti_client.py` (PC 역할, 접속하는 쪽)

### 5.5-2. 보드에 올리기 — ✅ 완료 2026-09-09

- [x] ~~**OS 이미지 기록(flash) 및 첫 부팅**~~ -> **완료** (2026-09-08)
- [x] ~~빌드 방식 결정~~ -> **개발 단계는 보드 네이티브 빌드**(2026-09-09 개통 완료).
      **제품은 바이너리 전용 배포로 전환** — 5.5-5 절 참고
- [x] ~~보드에 의존성 설치~~ -> **완료** (2026-09-09). `libsqlite3-dev libcjson-dev` (+`libc6-dev`)
- [x] ~~**eth0 살리기**~~ -> **완료** (2026-09-09). 케이블 연결 후 **1Gbps/Full**, PHY `RTL8211F`,
      DHCP로 **192.168.0.164/24**. eth0가 기본 경로(metric 100 < wlan0 600).
      개발 PC -> `192.168.0.164:1004` 접속 확인
- [x] ~~**eth0 MAC이 부팅마다 바뀐다**~~ -> **틀린 판단이었다. 문제 없음** (2026-09-09 재검증).
      `addr_assign_type = 0 (NET_ADDR_PERM)`이고 **재부팅 2회에도 MAC이 동일**했다.
      DHCP 임대도 유지돼 eth0가 다시 .164를 받았다. `0a:` 접두사(로컬 관리 주소)만 보고 랜덤이라고
      단정했던 것이 오판이었다
  - [ ] (제품 단계) IEEE OUI가 등록된 주소는 아니다. 자체 OUI를 쓸지는 출하 전 별도 판단
- [x] ~~**고정 IP 결정**~~ -> **eth0 = 192.168.0.250/24 고정** (GW/DNS 192.168.0.1). 2026-09-09.
      대역 스캔으로 빈 주소를 확인하고 정했다(`.200`은 이미 사용 중이었다)
- [x] ~~**webui에 IP 설정 넣기**~~ -> **완료** (2026-09-09). 화면/키보드가 없어 IP를 webui로 바꿔야 하는데
      잘못 넣으면 복구 수단이 없으므로 **세 겹으로 막았다**: ① `arping -D` 충돌 검사
      ② 확인 없으면 자동 롤백 ③ 확인 창 중 전원이 나가도 부팅 시 롤백.
      상세는 README "고정 IP + webui 네트워크 설정"
- [ ] **wlan0을 끌지 결정** — 제품은 이더넷 전용이다(사용자 확정). 개발 중에는 접속 경로가 두 개라
      **eth0 설정을 안전하게 바꿀 수 있는 안전망**이라 지금은 켜 둔다.
      제품에서는 불필요하고 공격면만 넓히므로 꺼야 한다
- [ ] **webui를 서비스로 올리기** — 지금은 Flask 개발 서버를 손으로 띄운다.
      네트워크 설정 기능이 들어갔으니 **webui가 없으면 현장에서 IP를 못 바꾼다**.
      5.5-5(소스 노출) 결정과 함께 처리
  - [ ] 보드에 Flask 미설치(`python3-venv` 없음)
  - [ ] `ACU_WEBUI_SECRET`이 기본값 `dev-only-change-me`다 — 배포 전 반드시 변경
  - [ ] 4자리 PIN + HTTP 평문. 네트워크 설정까지 다루게 됐으니 인증 강화가 더 급해졌다
- [x] ~~**시간 동기화**~~ -> **완료** (2026-09-09). `Asia/Seoul` (KST +0900), NTP 동기화 확인.
      이벤트 BCD 시각이 PC에서 정상으로 보이는 것까지 확인함
- [x] ~~**경로 절대화**~~ -> **완료** (2026-09-09).
      config는 `-c PATH` 옵션, PID/로그는 config.json의 `pid_path`/`log_path`(선택 필드),
      mock FIFO는 환경변수 `ACU_MOCK_FIFO`. cwd=`/`에서 동작 확인
- [x] ~~**systemd 유닛** + 부팅 시 자동 시작~~ -> **완료** (2026-09-09).
      `deploy/acud.service`, `deploy/install.sh`, `deploy/config.json`.
      전용 계정 `acud`로 실행, `kill -9` 후 자동 재시작 및 **실제 재부팅 후 자동 시작 확인**
  - [x] ~~**부팅 후 46초 지연**~~ -> **수정됨**. 유닛의 `Wants=network-online.target` 때문에
        `NetworkManager-wait-online`(38.5초)을 기다렸다. 출입통제 장치가 정전 복구 후 46초간
        카드를 못 읽는 것은 받아들일 수 없다. `After=network.target`으로 바꿔 **46초 -> 8초**.
        이 데몬은 네트워크를 기다릴 이유가 없다 — `bind(0.0.0.0)`은 주소가 붙기 전에도 성공하고
        `net_init` 실패해도 출입 판정은 계속 돈다
  - **교훈: `systemctl is-enabled`가 `enabled`인 것은 "부팅 시 자동 시작된다"의 증거가 아니다.**
        실제로 재부팅해 봐야 한다
  - [x] ~~**포트 1004 특권 포트**~~ -> **해결**. `AmbientCapabilities=CAP_NET_BIND_SERVICE`로
        일반 사용자 `acud`가 1004에 bind 되는 것을 실제로 확인했다 (root 실행 불필요)
- [x] ~~**로그를 stdout -> journald/파일**로~~ -> **완료** (2026-09-09).
      `log_open()/log_reopen()/log_close()` 추가. systemd에서는 stdout이 journald로 들어가므로
      유닛은 `log_path`를 비워 둔다. 파일 출력 시 SIGHUP에 재오픈(logrotate 대응)
- [ ] 웹UI를 보드에서 띄울지 -> **띄워야 한다**(네트워크 설정이 webui에 들어갔다). 방식은 5.5-5와 함께 결정
  - [x] ~~경로 준비~~ -> `ACU_CONFIG_PATH` / `ACU_PID_PATH` 환경변수 추가 (2026-09-09).
        데몬화하면 설정은 `/etc/acud/`, PID는 `/run/acud/`로 흩어져 기존 `ACU_ACUD_DIR` 하나로는 안 된다
  - [ ] 보드에 `python3-venv` 미설치

### 5.5-3. PC와 실제 통신 개통

- [x] ~~**포트를 1004로 맞출지 결정**~~ -> **1004로 결정** (2026-09-09).
      DM/Platinum 기본값에 맞췄다. 특권 포트 bind가 실제로 되는 것을 확인했고
      **보드는 지금 1004로 돌고 있다**(`/etc/acud/config.json`). `deploy/config.json`도 1004.
      개발용 `acud/config.json`은 9870 그대로다 — 유닛 밖에서 손으로 띄우면 능력이 없어 bind가 안 되기 때문
  - [ ] PC 프로그램을 실제로 붙여 확인하는 것은 아직 남아 있다
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

**첫 부팅 확인 결과 (2026-09-08)**

| 항목 | 값 | 판정 |
|------|-----|------|
| 커널 | `5.10.160-18-rk356x` | Rockchip BSP 5.10 — 의도대로 |
| 아키텍처 | `arm64` | OK |
| Debian | `11.8` (bullseye) | OK |
| `apt update` | **실패** | 원인 확인 필요 (아래) |
| `cdc_acm` | **있음** (`cdc-acm.ko.xz`, 로더블 모듈) | RRU 연결 시 자동 로드 — 6단계 준비 완료. (`modinfo` 최초 실패는 `/usr/sbin`이 PATH에 없어서였음) |

- [x] ~~이미지/커널/아키텍처 확인~~ (위 표)
- [x] ~~**`sudo sh tools/board_check.sh` 실행**~~ -> **개별 점검으로 대체 완료** (2026-09-09).
      시각/네트워크/저장소/의존성/cdc_acm을 개통 과정에서 모두 직접 확인했다.
      스크립트(`tools/board_check.sh`, 읽기 전용)는 다음 보드를 셋업할 때 쓰면 된다
- [x] ~~`cdc_acm` 재확인~~ -> **있음 (2026-09-08 확인)**.
      `/lib/modules/5.10.160-18-rk356x/kernel/drivers/usb/class/cdc-acm.ko.xz`, alias `char-major-166-*`.
      로더블 모듈이라 RRU를 꽂으면 자동 로드된다. **6단계 USB 경로의 OS 쪽 준비는 끝**
- [x] ~~시각 확인~~ -> **정상** (2026-09-08 10:04 UTC, 실제와 일치). NTP가 돌았다는 뜻이므로
      **네트워크도 살아 있을 가능성이 높다** -> apt 실패는 저장소 문제로 좁혀짐
- [x] ~~**타임존을 Asia/Seoul로 변경**~~ -> **완료** (2026-09-09). KST +0900, NTP 동기화 확인
- [x] ~~`apt update` 실패 원인 확정~~ -> **원인 2개 확정 (2026-09-08, 저장소를 직접 조회해 확인)**

  | 저장소 | 상태 | 문제 |
  |--------|------|------|
  | `bullseye` main | 정상 (Valid-Until 없음) | — |
  | `bullseye-updates` | 정상 (Valid-Until 없음) | — |
  | **`bullseye-security`** | **Release 만료** | `Valid-Until: 2026-09-07 21:13 UTC` — **어제 만료**. Bullseye LTS가 2026-08-31로 끝나 더 갱신되지 않는다. apt는 만료된 Release를 거부한다 |
  | **`bullseye-backports`** | **404** | 아카이브에서 제거됨 |
  | radxa / radxa-rockchip | 정상 (200) | — |

  -> **`deb.debian.org`는 아직 bullseye를 서비스하고 있다.** archive.debian.org로 옮길 필요 없음
     (오히려 archive에는 `debian-security/bullseye-security`가 없다)

  **조치 — 2026-09-09 실행 결과 정정.** 위 분석대로 하면 **설치가 실패한다.**
  `Check-Valid-Until`을 꺼서 security Release를 되살려도 **풀의 .deb가 이미 삭제돼 있고**
  (`libc-dev-bin_..._deb11u14_arm64.deb` -> 404), apt가 모든 candidate를 security 버전으로 잡기 때문에
  설치가 통째로 막힌다. **security 저장소도 비활성화해서 bullseye main 버전으로 떨어뜨려야 한다.**
  ```bash
  # 1) 사라진 backports 비활성화
  sudo mv /etc/apt/sources.list.d/bullseye-backports.list \
          /etc/apt/sources.list.d/bullseye-backports.list.disabled

  # 2) 풀이 비어 있는 security 저장소도 비활성화   <- 이것이 핵심
  sudo mv /etc/apt/sources.list.d/bullseye-security.list \
          /etc/apt/sources.list.d/bullseye-security.list.disabled

  # 3) (있어도 무해) 만료 Release 허용
  echo 'Acquire::Check-Valid-Until "false";' \
      | sudo tee /etc/apt/apt.conf.d/99no-check-valid-until

  sudo apt update
  sudo apt install -y libsqlite3-dev libcjson-dev
  ```
  - security를 끄는 것이 보안 후퇴로 보이지만 **그 저장소는 지금 설치 가능한 패키지를 하나도 제공하지
    못한다**(전부 404). 실질적으로 잃는 것이 없다. 대신 **bullseye EOL이 확정 사실**임을 확인해 준 것이므로
    제품용 배포판 재선정은 미룰 수 없다
  - 실제 설치된 버전: `libsqlite3-dev 3.34.1-3`, `libcjson-dev 1.7.14-1+deb11u1`,
    `libc6-dev 2.31-13+deb11u11`
  - **`build-essential`/`python3-venv`는 설치하지 않았다** — gcc/make는 이미 있었고 acud 빌드에 불필요.
    webui를 보드에서 띄울 때 `python3-venv`가 필요해질 수 있다
- [ ] (예비) **apt가 끝내 안 되면 우회** — 데몬이 필요한 건 헤더 2개(`sqlite3.h`, `cJSON.h`)뿐이다.
      보드에는 이미 `libsqlite3.so.0`이 있고 gcc/make/git/python3(3.9.2)/venv/pip3도 깔려 있다.
      (a) arm64 `.deb`를 개발 PC에서 받아 `dpkg -i`, (b) 소스를 저장소에 vendoring,
      (c) 개발 PC에서 크로스 빌드해 바이너리만 복사. **apt가 죽어도 pip(PyPI)는 될 수 있으니
      웹UI는 별개로 판단할 것**
- [ ] 기본 계정/비밀번호 -> **`rock`/`rock` 그대로임을 2026-09-09 확인. 변경 필요**
- [x] ~~**의존성 설치**~~ -> **완료** (2026-09-09). acud 빌드에는 `libsqlite3-dev libcjson-dev`면 충분했다.
      webui를 보드에서 띄울 때 `python3-venv`가 추가로 필요
      (bullseye의 pip은 오래돼서 Flask 3.x 설치 전에 `pip install -U pip` 필요할 수 있음)
- [ ] **XFCE 데스크톱 끄기** — 우리 제품은 headless다. `systemctl set-default multi-user.target`으로
      디스플레이 매니저를 내리면 메모리/부팅시간이 준다.
      **eMMC(7.3G) 이전의 선행 조건이기도 하다** — 패키지까지 제거하면 용량 여유가 크게 생긴다
- [x] ~~**설치 매체 결정**~~ -> **당분간 SD 유지, eMMC 이전은 6단계(RRU 개발보드) 착수 전에** (2026-09-09 결정)

  실물 확인 결과:

  | 장치 | 정체 | 크기 | 상태 |
  |------|------|------|------|
  | `mmcblk0` | **SD 카드** (SP32G) | 29.7G | **부팅 중** — `/`, `/boot/efi`, `/config` |
  | `mmcblk1` | **eMMC** (DG4008) | **7.3G** | **비어 있음** (파티션 없음) |

  - **지금 SD로 계속하는 근거**: 진행 중인 작업(systemd 유닛, install.sh, 경로 절대화)은
    **부팅 매체와 무관**하다. eMMC로 옮겨도 `install.sh`를 다시 돌리면 끝이라 재작업이 없다.
    반면 eMMC 기록은 USB-C OTG + maskrom(`rkdeveloptool`) 경로가 필요해 개발 속도만 떨어진다.
    SD는 망가뜨려도 다시 구우면 된다
  - **끝까지 미루면 안 되는 이유**: **eMMC 7.3G vs SD 29.7G.** 현재 rootfs 사용량은 3.7G라
    여유가 있지만, 30G 환경에서 개발하다 마지막에 7.3G에 안 들어가는 것을 발견하면 곤란하다.
    지금 이미지에는 **XFCE 데스크톱이 통째로 들어 있다**(제품은 headless)
  - **제품에는 eMMC가 필수다** — SD 카드는 진동/전원 차단/마모로 죽는다.
    출입통제 장치가 SD 불량으로 멈추면 사고다
- [ ] **eMMC 이전** — 6단계 착수 전에 수행. 실제 하드웨어 통합과 부팅 매체 디버깅을 동시에 하지 않기 위함
  - [ ] rootfs를 7.3G 안에 넣기 (XFCE 제거가 핵심)
  - [ ] USB-C OTG + maskrom + `rkdeveloptool` 경로 확보
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
- [ ] **보드의 radxa 저장소 GPG 키 만료** (2026-09-09 확인) — `radxa-repo.github.io`의 두 저장소가
      `NO_PUBKEY 67A474DD40402951`, `NO_PUBKEY 5D93177D0752732A`로 서명 검증에 실패해 `apt update`가
      에러를 낸다. 우리 패키지는 전부 Debian main에서 오므로 **빌드에는 영향 없음**.
      커널/BSP 패키지를 apt로 갱신해야 할 때 문제가 된다
- [ ] **Debian bullseye EOL 확정** — security 저장소의 .deb가 실제로 삭제된 것을 확인했다(404).
      제품 출하용 배포판 재선정이 미룰 수 없는 과제가 됐다 (5.5-4 마지막 항목과 연결)

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
