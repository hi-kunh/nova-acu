# 작업 목록 (TODO)

ACU 프로젝트의 진행 상황과 남은 일 정리.
완료된 단계의 **상세 기록은 [README.md](README.md)** 에 있고, 이 파일은 "무엇이 남았는지"만 관리한다.

- 최종 갱신: 2026-09-07
- 현재 위치: **5단계 완료 / 6단계 착수 예정**

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
| 6 | 실제 하드웨어 (USB, ACU↔RRU) | ⬜ 다음 작업 |
| 7+ | 배포/운영 (systemd, 보안 강화 등) | ⬜ 미착수 |

범례: ✅ 완료 · 🔶 부분 완료(남은 항목 아래 참고) · ⬜ 미착수

---

## 다음 작업: 6단계 — RRU 연동 (USB)

하드웨어 구성이 확정되면서 6단계 내용이 바뀌었다. **RK3566에서 Wiegand를 직접 받는 것이 아니라,
별도 RRU 보드(리더8 / 입력24 / 출력8)와 USB로 통신**한다. 구성 상세는 [HARDWARE.md](HARDWARE.md).

### 6-0. 설계 확정 (코드 작성 전)

- [x] ~~RRU 대수~~ → **1대 고정 (point-to-point)**, ~~설치 형태~~ → **같은 함체 내부** (확정)
  - 결과: 프레임에 **장치 주소 필드 불필요**, 카드 이벤트는 **RRU 능동 송신**
  - 물리 계층이 USB든 UART든 이 두 결론은 그대로 유지됨
- [x] ~~baud/프레이밍~~ → **115200 8N1** (확정)
- [x] ~~물리 계층~~ → **USB 확정** (IO 보드 J18 = 2.54mm 10핀 내부 USB 헤더 사용).
      UART의 핀먹스/콘솔 충돌 문제가 전부 사라짐. UART 조사 기록은 HARDWARE.md에 참고용으로 보존
- [x] ~~RRU MCU 선정~~ → **STM32C562RE / NUCLEO-C562RE** 개발보드로 시작
  - 보드에 네이티브 USB Type-C Device 커넥터 내장 → CDC-ACM, `/dev/ttyACM0`. 브릿지 IC 불필요
  - ST-LINK VCP가 별도 채널로 남아 RRU 디버그 로그를 프로토콜 링크와 분리 가능
  - CAN FD 커넥터(CN18)도 있어 나중 RK3568+CAN 경로를 같은 보드에서 검증 가능
  - 주의: USB-C로 보드 급전 불가(self-powered 전제) → VBUS/GND 처리 정할 것
- [ ] **최종 RRU 보드 I/O 설계** — LQFP64로는 48 I/O(Wiegand16+입력24+출력8)가 한계 초과.
      Wiegand D0/D1 다이오드 OR, 입출력 익스팬더, 상위 패키지 등 검토
- [ ] 개발보드 단계는 **리더 1~2채널만 붙여 프로토콜 검증** (8채널은 실보드에서)
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
- [ ] 8리더 각각 구분되어 판정되고, 이벤트의 Reader Address가 올바른지
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
