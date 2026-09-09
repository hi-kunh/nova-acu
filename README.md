# ACU (Access Control Unit)

임베디드 리눅스 기반 출입보안 장치. 화면 없는(headless) CLI 데몬으로 동작한다.
기존 STM32 기반 제품(IDTi 시리즈)과 동일한 통신 프로토콜을 사용하여, RK3566/RK3568 기반으로 재구현하는 프로젝트.

> 남은 작업 목록과 다른 PC에서 이어서 작업하는 방법은 **[TODO.md](TODO.md)** 참고.
> 이 README는 완료된 단계의 상세 기록을 남기는 용도.

## 개발 단계

- **[0단계] 데몬 뼈대**  완료 - `acud` 폴더
- **[1단계] SQLite3 + 출입 판정 로직**  완료 (아래 "1단계 완료 기록" 참고)
- **[2단계] 하드웨어 추상화 계층(HAL) 인터페이스 확정**  완료 (아래 "2단계 완료 기록" 참고)
- **[3단계] config.json 감시 + 무중단 리로드**  완료 (아래 "3단계 완료 기록" 참고)
- **[4단계] 웹 설정 인터페이스 (Flask)**  완료, 1차 범위(config.json 편집)만 (아래 "4단계 완료 기록" 참고)
- **[5단계] 네트워크 통신부 (TCP, IDTi 프로토콜 V2)**  완료, 1차 범위(Event Log 조회 응답)만 (아래 "5단계 완료 기록" 참고)
- **[5.5단계] ACU 단독 개통 (CM3 보드 + PC 통신)** - 진행 중. RRU 개발보드를 기다리는 동안
  acud를 실제 CM3 IO 보드에 올리고 상위 시스템 PC와 TCP 통신을 개통한다 (아래 "5.5단계 진행 기록" 참고)
- **[6단계] 실제 하드웨어** - 착수 예정. RK3566(ACU) ↔ USB ↔ RRU 보드 (아래 "시스템 구성" 참고)
  - RRU MCU는 **STM32C562** 확정. 외부 인터럽트는 Wiegand 16입력(리더 8 x D0/D1)에 **충분함 확인**
  - **개발 순서 확정**: NUCLEO-C562RE 개발보드 구매 → **8모듈 중 2모듈만 구현해 검증** →
    검증 후 본 제품(8모듈) 보드 설계. 개발보드에 8모듈분 I/O가 없기 때문
  - RK3568 + CAN 경로는 이번 범위에서 제외 (보드 확보 후 별도 진행)

## 대상 하드웨어

- 기존 제품: STM32 기반, IDTi 프로토콜로 통신
- 신규 목표: RK3566 또는 RK3568
- **현재 보유 보드**: RK3566 기반 **Radxa CM3 IO Board**
- **보드 OS 확정**: Radxa 공식 **Debian Bullseye (XFCE) b25** —
  `radxa-cm3-io_debian_bullseye_xfce_b25.img.xz`. 확인 항목은 [TODO.md](TODO.md) "5.5-4" 참고
- 최종적으로 기존 제품과 100% 동일한 IDTi 프로토콜로 통신 가능하게 만드는 것이 목표 (기존 상위 시스템/웹앱과 호환)

## 시스템 구성

```
[상위 시스템] --TCP/IDTi V2--> [ACU: RK3566] --USB--> [RRU: 리더8 / 입력24 / 출력8]
```

리더기·센서·릴레이는 ACU에 직접 연결되지 않고 별도의 **RRU 보드**가 담당한다.
ACU는 USB로 RRU와만 통신하므로, 6단계는 "RK3566에서 Wiegand를 직접 받는 일"이 아니라
**"ACU-RRU 간 통신 프로토콜을 설계·구현하는 일"** 이다.

개발 단계에서는 개발보드 I/O 제약 때문에 **2모듈(도어 2개분: 리더2 / 입력6 / 출력2)** 로만 검증한다.
모듈 1개 = 리더1 + 입력3(Exit/Door/Lock) + 출력1. **소프트웨어(acud)는 처음부터 8채널 전제로 만들고**,
2모듈은 하드웨어 검증 범위의 제약일 뿐이다.

> 보드 사양, 채널 구성표, IDTi 매핑, 2모듈 검증 계획, 미확정 사항은
> **[HARDWARE.md](HARDWARE.md)** 에 별도로 정리.

## 참고 프로토콜 문서 (기존 장치 통신 규격)

경로: `/home/jayden/workspace/idti/IDTI WebApp Protocol/`

이 프로젝트가 반드시 호환해야 하는 기존 IDTi 프로토콜 문서 모음. 지금까지 확인한 문서:

- `1. IDTi Protocol V1_2_3 Basic structure.doc` — 패킷 프레임 기본 구조 (5단계 네트워크 통신부 핵심 참고)
- `2. IDTi Protocol Event Structure & Event Code.doc` — Event Log(History) 구조와 Event Code 표 (5단계 참고)
- `4. IDTi Protocol TimeZone & Holiday & Validation.doc` — 유효기간/시간대 그룹 구조 (1단계 참고)
- `5. IDTi Protocol UserBinaryTransmit.doc` — 유저 DB 대량 전송(바이너리) 프로토콜
- `7. IDTi Protocol Device Type Table.doc` — 장치 타입 코드 (2단계 HAL 참고)
- `9. IDTi Protocol Relay(Output) & Sensor(Inout) Function.doc` — 릴레이/센서 구조 (2단계 HAL 참고)

그 외 폴더 내 문서(향후 단계에서 필요할 때 참고):
`3. Member(User DB) Structure`,
`6. FileBinaryTransmit`, `8. System Device Reader Setup`,
`10. Group`, `11. User General Group`, `12. Canteen`,
`13. Force OpenMode`, `14. Request Blocking User List`, `15. Elevator Group Setup`,
`IDTi HW Device_Directory File Structure`, `IntelliScan Web Application Project`

> `.doc`(구 바이너리 워드 포맷)라 바로 안 열림 → `libreoffice --headless --convert-to txt --outdir <홈 디렉토리 하위 경로>`로 변환해서 읽음.
> **주의**: libreoffice snap이 `/tmp`나 프로젝트 스크래치패드 등 홈 바깥 경로에는 못 씀 (permission denied) — 반드시 `$HOME` 하위 경로를 `--outdir`로 지정할 것.

### 프로토콜 핵심 요약 (Basic structure 문서)

**패킷 프레임 구조** (V2/V3 기준)

```
Header(44 Byte) + Data(N Byte) + Tail(2/4 Byte)
```

Header 필드: `STX(1) Packet Length(2) Protocol Version(1) Frame Option(2) Header Length(1) Address(13) Frame Index(4) Password(4) Command(3) Header Checksum(1) Object(2) Item Index(2) Data Block Index(8)`

Tail 필드: `Packet CheckBytes(0/2, XOR+SUM) Packet Checksum(1, 고정) ETX(1, 고정=0x03)`

- STX = `0x02` 고정
- Command = Command Option(1) + Command(1) + Sub Command(1)
  - Command: `Snd Status 0x03 / Req Status 0x04 / Snd Data 0x05 / Req Data 0x06`
  - Sub Command: `Read 0x02 / Write 0x03 / Delete 0x04 / Change 0x05 / Init 0x06`
- Object (카드/출입 판정 관련 핵심):
  - `UserAll 0x15` (ID+LCD Name+Card+Finger), `UserInfo 0x16`, `UserCard 0x17`, `UserFinger 0x18`, `UserData 0x21`
  - `History 0x01` (이벤트 로그), `DeviceValidation 0x69`, `DeviceTimezone 0x68`, `DeviceHoliday 0x6C`
- User Info(32byte) 구조: `User ID(8) + Revision(4) + User Option(4, bitflag: Enable/Timezone/Validation 등) + Level(1) + Validation Code(2) + Timezone Code(2) + ... + Card Type(2) + Finger Type(2) + Finger Count(1) + Password(2) + Canteen(1)`
- 이벤트 로그(History) 안에 `"Access Authorized By Card"` 같은 Event Code + User/Card ID(8byte) + Date/Time 포함 → **출입 판정 결과를 이 이벤트 구조로 기록해야 상위 시스템과 호환됨**

### User Binary Transmit 요약 (대량 유저 DB 전송, `5.` 문서)

- Object `0xD0`(Start) / `0xD1`(Continue) 로 대용량 유저 DB를 청크 단위로 송수신
- 유저 레코드 구조체는 모델별로 다름 (`_BSCUserInfo` 1772byte / `_ISCUserInfo` 128byte / `_SSCUserInfo` 128byte / `_SSCUserInfoFinger` 1808byte)
- 카드 전용(지문 없음) 모델 기준이면 `_ISCUserInfo`(128byte, `serial(4)+user(32)+card(12)+name(16)+restrict(8)+grpcode(16)+apb(1)+reserved(37)+crc_calc(1)+datacrc(1)`) 구조가 가장 근접 — **우리 카드 전용 데몬 스키마 설계 시 참고**

### Device Type Table 요약 (`7.` 문서)

- 장치를 카테고리별 1byte 코드로 구분: Server/Workstation/Com Slot, Controller(SSC 0x1f~/ISC 0x29~/BSC 0x33~), Built-In-Module, Remote Module(RIM/ROM/RRM/RXM), Reader(0x8d~0x91), Relay(0x97~0xaa), Sensor(0xab~0xff)
- Sensor 카테고리 중 우리 장치와 관련 있는 것: `Door Contact 0xac`, `Lock State 0xad`, `Door Control(Exit Button) 0xae`
- 우리 컨트롤러가 프로토콜상 어떤 타입 코드로 응답할지는 아직 미확정 (Controller(ISC) 계열 0x29~0x32가 카드 전용 컨트롤러로 가장 근접, HAL 헤더에는 참고용으로 `HAL_DEVICE_TYPE_ISC101 0x29`만 임시로 남겨둠)

### Relay & Sensor 요약 (`9.` 문서)

- **Device Output(Relay)**: Object `0x2D`, 채널 Index 1~254. 구조체 핵심 필드: `IsEnabled`, `ActiveType`(1=Door, 2=Alarm, 3=LockDown, 4=Continuous, 5=Time_Relay, 6=Fail Relay, 7=Door Status Relay, 8=Solenoid Relay), `ActiveTime`(1~99초)
  - 우리 장치는 **Door Relay(ActiveType=1)** 만 사용: 인증 성공 시 `ActiveTime`초 동안 릴레이 동작
    (구성 변경으로 도어 8개 -> 릴레이 8채널. 채널별 NO/NC 선택은 [HARDWARE.md](HARDWARE.md) 참고)
- **Device Input(Sensor)**: Object `0x2C`, 채널 Index 1~254. 구조체 핵심 필드: `Use`(Exit/Alarm/Lock/Door/Intrusion/NoAction), `ActiveType`(0=Normal Close, 1=Normal Open)
  - 구성 확정 후: 도어당 `Exit Button`/`Door Contact`/`Lock State` 3종 x 8도어 = 24채널 (HARDWARE.md 참고)

## 1단계 완료 기록 (SQLite3 + 출입 판정 로직)

**구성**
- `acud/log.h`, `acud/log.c` — 타임스탬프 로그 출력 (`main.c`/`access.c` 공용)
- `acud/db.h`, `acud/db.c` — SQLite3 open/close, 스키마 초기화, 카드/유효기간/시간대 조회
- `acud/access.h`, `acud/access.c` — 조회 결과로 출입 허용/거부 판정
- `acud/main.c` — 실제 리더기(6단계 예정)가 없으므로, 더미 카드ID 6개를 순회하며 판정 로직을 검증

**DB 스키마** (IDTi User Info(32byte) 필드 대응, 카드 전용 모델 기준)
- `cards`: `card_id, user_id, is_enabled, level, validation_code, timezone_code`
- `validations` (IDTi Validation ID 1~1024 대응): `validation_id, start_date, end_date` — 유효기간 그룹. `validation_code=0`이면 그룹 없음(무조건 유효)
- `timezones` (IDTi Timezone ID 1~1024 대응, 그룹당 슬롯 최대 4개): `timezone_id, slot_index, start_hour, start_min, end_hour, end_min, week_select` — `week_select`는 16bit 비트마스크(`bit15=일 ... bit9=토`, `bit8~0=공휴일1~9`, `4. TimeZone & Holiday & Validation.doc` 참고). `timezone_code=0`이면 그룹 없음(무조건 허용)
- **공휴일 캘린더(Device Holiday)는 아직 미구현** — `week_select`의 공휴일 비트(bit8~0)는 반영되지 않음, 요일 비트만 확인함 → 향후 필요 시 별도 단계로 구현

**판정 순서** (`access_judge`): 카드 조회 → 비활성화 여부 → 유효기간(Validation) → 시간대(Timezone) 순으로 확인, 하나라도 걸리면 즉시 거부

**검증 방법**: 더미 카드 6개로 허용/비활성/미등록/유효기간만료/그룹통과/시간대불일치 6가지 경로를 모두 실행 확인함 (`make run` 후 로그 출력으로 확인)

**빌드 전제조건**: `libsqlite3-dev` 설치 필요 (`sudo apt-get install libsqlite3-dev`) — 이번 세션에서 설치 확인 완료

## 2단계 완료 기록 (HAL 인터페이스 확정)

**구성**
- `acud/hal.h` — 카드 리더 입력(`hal_read_card`) / 도어 릴레이 출력(`hal_open_door`) / 센서 입력(`hal_read_sensor`) 인터페이스 정의
- `acud/hal_mock.c` — 실제 GPIO/Wiegand(6단계 예정)가 없는 동안의 모의 구현체. 1단계 때 `main.c`에 있던 더미 카드ID 6개 순회 로직을 여기로 옮김
- `acud/main.c` — 더 이상 더미 카드 배열을 직접 참조하지 않고, `hal_read_card()`로 카드를 받고 허용 시 `hal_open_door()`를 호출하도록 변경

**설계 원칙**: `main.c`/`access.c`는 `hal.h` 인터페이스만 알고, 실제 하드웨어 유무와 무관하게 동일하게 동작한다. 6단계에서 `hal_rk3566.c`/`hal_rk3568.c`(실제 GPIO/Wiegand 구현체)로 교체할 때 `hal_mock.c`만 빼고 갈아끼우면 되도록 함

**IDTi 프로토콜 대응**: HAL의 릴레이 동작은 Device Output(Relay) Object(`0x2D`)의 `ActiveType=1(Door Relay)` 의미를 따름 (`ActiveTime`은 현재 3초 상수, 3단계 config.json 도입 후 설정값으로 전환 예정). 센서 채널은 `Door Contact`/`Exit Button`만 우선 정의(그 외 Sensor Device Type은 필요 시 확장)

**검증 방법**: `make run`으로 실행, 허용된 카드에서만 "HAL(mock): 도어 릴레이 3초 동작" 로그가 남고 거부된 카드는 릴레이가 동작하지 않음을 확인함

## 3단계 완료 기록 (config.json 감시 + 무중단 리로드)

**구성**
- `acud/config.h`, `acud/config.c` — `config.json`을 [cJSON](https://github.com/DaveGamble/cJSON)으로 파싱해 `AcuConfig`(`db_path`, `door_open_seconds`)에 채움. 파일이 없거나 파싱 실패 시 기존 값을 그대로 유지(안전한 실패)
- `acud/config.json` — 실행 파일과 같은 디렉터리에 두는 기본 설정 파일 (커밋 대상, DB 파일 자체와 달리 `.gitignore` 대상 아님)
- `acud/main.c` — 시작 시 `config_set_defaults()` → `config_load()` 순으로 로드. `SIGHUP`을 받으면 `config_load()`를 다시 호출해 재시작 없이 적용: `door_open_seconds`는 다음 판정부터 바로 반영, `db_path`가 바뀌면 기존 DB를 닫고 새 경로로 다시 열어 교체

**유효성 검사**: `door_open_seconds`는 IDTi Device Output(Relay)의 `ActiveTime` 범위(1~99초)를 벗어나면 리로드를 거부하고 기존 설정을 유지함

**검증 방법**: 데몬 실행 중 `config.json`의 `door_open_seconds`를 3→10으로 바꾸고 `kill -HUP`을 보내 재시작 없이 "HAL(mock): 도어 릴레이 10초 동작"으로 즉시 바뀌는 것을 확인함. `config.json`을 삭제한 채 기동해도 기본값(3초)으로 정상 동작함을 확인함

**빌드 전제조건**: `libcjson-dev` 설치 필요 (`sudo apt-get install libcjson-dev`) — 이번 세션에서 설치 확인 완료

## 4단계 완료 기록 (웹 설정 인터페이스, 1차 범위: config.json 편집만)

**범위**: 카드 관리 화면은 이번엔 제외하고, `acud/config.json`(`db_path`, `door_open_seconds`) 편집 화면만 우선 구현. 카드 CRUD는 추후 단계에서 추가 예정

**구성**
- `webui/app.py` — Flask 앱. `acud/config.json`을 직접 읽고 씀. 저장 시 `acud/acud.pid`에 적힌 PID로 `SIGHUP`을 보내 acud가 재시작 없이 반영하도록 함 (3단계 무중단 리로드 기능을 그대로 활용)
- `webui/templates/index.html` — 설정 폼 1페이지 (db_path, door_open_seconds, 단말기 비밀번호 변경)
- `webui/templates/login.html` — 로그인 화면 (숫자 4자리 단말기 비밀번호)
- `webui/requirements.txt` — `Flask>=3.0,<4`
- `acud/main.c` — 시작 시 `acud.pid`에 PID 기록, 정상 종료 시 삭제 (webui가 SIGHUP 보낼 대상을 찾기 위함)

**유효성 검사**: `door_open_seconds`는 웹 폼에서도 1~99(IDTi Relay ActiveTime 범위)를 벗어나면 저장하지 않고 에러만 표시 (config.c의 검증과 동일한 규칙을 웹에서도 반복 적용)

**로그인 게이트 (단말기 비밀번호)**: 설정 화면 진입 전, `config.json`의 `admin_password`(숫자 4자리, 기본값 `"0000"` — **반드시 변경 필요**)로 로그인해야 함. IDTi Header의 `Password(4byte)` / User Info의 `Password(2byte BCD)` 필드와 같은 맥락의 "단말기 비밀번호" 개념으로, 호스트 PC 디바이스 관리 매니저 프로그램의 비밀번호 설정 기능에 대응시킴
- `acud/config.h`/`config.c`: `AcuConfig.admin_password` 필드 추가, 숫자 4자리가 아니면 리로드 자체를 거부(다른 필드와 동일한 fail-safe 규칙)
- `webui`: Flask `session`으로 로그인 상태 관리 (`login_required` 데코레이터), 설정 화면에서 새 비밀번호(+확인) 입력 시에만 변경, 저장하면 다른 설정과 함께 `config.json`에 반영되고 SIGHUP으로 acud에도 전파됨
- 로그인 실패 시 1초 지연만 두어 무차별 대입을 최소한으로 늦춤

**보안 관련 미해결 사항 (중요)**: 숫자 4자리 PIN은 경우의 수가 10000개뿐이라 그 자체로 약하다. 실패 횟수 제한/계정 잠금, HTTPS, CSRF 방어, 사내망 한정 바인딩은 아직 없음 — 지금은 로컬 개발/검증 목적으로만 사용, 실제 배포 전 반드시 강화 필요

**검증 방법**: `acud` 실행 → `webui` Flask 개발 서버 실행 → 로그인 없이 `/` 접근 시 `/login`으로 리다이렉트됨을 확인 → 기본 비밀번호(`0000`)로 로그인 → 폼으로 `door_open_seconds`를 3→7로 저장 → acud 로그에 "설정 리로드 완료"가 찍히고 실제 도어 릴레이 동작 시간이 바뀜을 확인. 범위를 벗어난 값(150)을 보내면 config.json이 바뀌지 않고 에러 메시지만 표시됨을 확인. 새 비밀번호(1234)로 변경 후 config.json에 반영되고 acud가 리로드됨을 확인

**실행 방법**
```bash
cd webui
python3 -m venv .venv
./.venv/bin/pip install -r requirements.txt
./.venv/bin/python app.py   # http://localhost:5000
```

**빌드 전제조건**: `python3-venv` 설치 필요 (`sudo apt-get install python3-venv`) — 이번 세션에서 설치 확인 완료

## 5단계 완료 기록 (네트워크 통신부, 1차 범위: Event Log 조회 응답만)

**범위**: 상위 시스템(PC)이 TCP로 접속해 Event Log(History, Object `0x01`)를 요청하면 출입 판정 결과를
IDTi Event Structure(36byte)로 응답하는 것까지만 구현. 유저 DB 송수신(UserBinaryTransmit), Device Output
원격 제어, Time Sync 등 그 외 요청은 로그만 남기고 무시함 (추후 단계에서 필요한 것만 확장 예정). UDP는
프로토콜 문서상 RRE/Alarm Server 같은 부가 기능에만 쓰이고 PC-Controller 핵심 통신은 Frame Option의
`IsTCP` 비트가 있는 TCP 쪽이라 이번 단계는 TCP만 구현함.

**프로토콜 버전**: V2(44byte 헤더, Address 13byte, Device Status 234byte)를 사용. V1은 헤더가 작지만 상위
시스템과의 호환성 관점에서 V2/V3가 더 널리 쓰이고, V3(Device Status 1100byte, 최대 128리더)는 우리
단일 도어/단일 리더 장치에는 과함 - V2가 딱 맞는 절충점. IO 확장 모듈이 없는 장치이므로 Device Status의
`ExistedModule` 비트마스크는 항상 0, IO Module 배열(14개 * 16byte)은 전부 0으로 채움.

**구성**
- `acud/protocol.h`, `acud/protocol.c` — IDTi V2 패킷 프레임(Header 44byte/Tail 2byte) 파싱·생성, Header
  Checksum(XOR) 계산, BCD 변환. 실제 소켓 코드와 분리해 두어 유닛 테스트하기 쉽게 함
- `acud/net.h`, `acud/net.c` — TCP 서버(동시 1개 연결만 지원, select 기반 non-blocking), 32개 링버퍼로 된
  이벤트 큐, Event Log 요청 처리(Device Status + 대기 중인 이벤트 1건을 얹어 응답)
- `acud/config.h`/`config.c`/`config.json` — `tcp_port` 필드 추가(기본값 9870, 1~65535 검증). SIGHUP으로
  포트가 바뀌면 기존 리스닝 소켓을 닫고 새 포트로 재개설(재시작 없이 전환), DB 경로 전환과 동일한 패턴
- `acud/main.c` — `hal_read_card()`/`access_judge()` 결과를 `net_push_event()`로 큐에 넣고,
  `net_poll()`로 접속/요청을 처리. 도어 센서 상태(`hal_read_sensor`)도 매 조회 시각마다 갱신해 Event
  Info의 Door Status 필드에 반영
- `webui/app.py` — `tcp_port`가 저장/전달 과정에서 유실되지 않도록 기본 설정 목록에 추가만 함 (편집 화면은
  아직 없음, 카드 CRUD처럼 추후 단계에서 필요해지면 추가)

**Event Code 매핑** (`2. IDTi Protocol Event Structure & Event Code.doc` 참고)
- `ACCESS_GRANTED` → `Access Authorized By Card`(`0x01010102`), Access ID = User ID
- `ACCESS_DENIED_DISABLED` → `Access Denied By Not Enabled`(`0x01020108`)
- `ACCESS_DENIED_TIMEZONE` → `Access Denied By Time`(`0x01020109`)
- `ACCESS_DENIED_NOT_FOUND`/`ACCESS_DENIED_VALIDATION` → 문서에 더 구체적인 코드가 없어 일반 코드인
  `Access Denied By Card`(`0x01020102`)로 대체. 위 세 가지를 제외한 거부 사유는 모두 Access ID = Card ID
- `ACCESS_DENIED_DB_ERROR`는 내부 오류라 상위 시스템에 보고할 실질적 의미가 없어 이벤트 큐에 넣지 않음
  (로컬 로그로만 남김)

**이벤트 큐**: 카드 판정마다 매번 큐에 넣는다(허용/거부 모두). 32개 고정 크기 링버퍼이며, 가득 차면 가장
오래된 이벤트를 버리고 계속 진행(fail-safe, 상위 시스템이 오래 접속하지 않으면 오래된 이벤트부터 유실됨).
한 번의 History 요청에 이벤트 1건만 실어 응답함(Data Block Index의 Current/End/Total을 전부 1로 표기) -
여러 건을 한 응답에 몰아 보내는 것은 범위 밖으로 남겨둠.

**응답 주소 처리 (단순화)**: TCP는 1:1 연결이라 IDTi 프로토콜이 원래 상정한 RS-485 다중 장치 버스 주소
지정이 실질적 의미가 없다. 응답의 Destination Address는 요청의 Source Address(5byte)를 그대로 앞
5byte에 옮기고 나머지 3byte(Device 확장 비트마스크)는 0으로 채우며, Source Address는 문서상 고정값
(Host/ComSlot/Controller/Module/Device 전부 `0x01`)을 그대로 사용함.

**설계 결정: 카드 폴링 주기와 네트워크 처리 분리**: 처음에는 기존 `sleep(2)` 자리를 그냥 `net_poll(net, 2000)`
으로 바꿨는데, `select()`가 상위 시스템 요청이 들어올 때마다 즉시 반환하는 바람에 요청이 몰리면
`hal_read_card()`가 지연 없이 계속 호출되어 카드 폴링 주기가 무너지고 이벤트 큐가 순식간에 가득 차는
문제를 테스트 중 발견함. `CLOCK_MONOTONIC` 기준으로 다음 카드 조회 시각을 별도로 관리하고, `net_poll()`의
timeout을 "다음 카드 조회까지 남은 시간"으로 넘기는 방식으로 고쳐 두 주기를 분리함 - 네트워크 트래픽과
무관하게 카드 조회는 항상 2초 주기를 유지함.

**검증 방법**: 파이썬으로 실제 IDTi V2 요청 패킷(Header Checksum 포함)을 직접 만들어 보내는 테스트
클라이언트로 확인함
- Req Data(`0x06`)/Read(`0x02`)/History(`0x01`) 요청 → 응답이 `Header(44)+DeviceStatus(234)+Data(36)+Tail(2)
  = 316byte`(문서에 명시된 크기와 정확히 일치)로 오고, Event Code/Door Status/Access ID/BCD 시각이 모두
  올바르게 디코딩됨을 확인
- 큐가 빈 상태에서 요청하면 `Header(44)+DeviceStatus(234)+Tail(2) = 280byte`(Event Log 없는 경우의 문서
  크기와 일치), Data Block Total = 0으로 응답함을 확인
- 빠르게 연속 요청을 보내는 동안에도 `acud` 로그의 카드 판정 타임스탬프가 여전히 2초 간격을 유지함을 확인
  (위 "카드 폴링 주기 분리" 버그가 고쳐졌는지 검증)
- `kill -HUP`으로 `config.json`의 `tcp_port`를 9870→9871로 바꾸면 기존 포트(9870)는 연결이 거부되고 새
  포트(9871)로만 접속되며, `door_open_seconds`도 함께 재시작 없이 반영됨을 확인
- 잘못된 Header Checksum을 가진 패킷을 보내면 수신 버퍼를 초기화하고 연결은 끊지 않음을 확인

## 5.5단계 진행 기록 (ACU 단독 개통: CM3 보드 + PC 통신)

RRU 개발보드가 오기 전까지, **acud를 실제 CM3 IO 보드에서 돌리고 상위 시스템 PC와 통신을 개통**하는 작업.
남은 항목은 [TODO.md](TODO.md)의 "5.5단계" 절에서 관리한다.

### 확정 사실 - 연결 방향과 상대 프로그램 (2026-09-08)

기존 PC 프로그램 소스가 로컬(`/home/jayden/workspace/idti/`)에 있어 직접 확인함.

- 상위 시스템은 **IntelliScan NET Platinum**이고, `clsAsynchronousClient.cs`에서 `Socket.BeginConnect`로
  **장치에 접속해 오는 TCP 클라이언트**다. 즉 **ACU가 listen 하는 서버가 맞다** (현재 구조 유지)
- Platinum의 기본 포트는 **1004** (`clsAsynchronousClient.cs:35`). acud 기본값 9870과 다르므로 맞춰야 함
- 프레임/이벤트 구조는 `IntelliScan Device SDK Project/Source/isldev/`의 `clsDevFrame.cs`,
  `clsDevFrameAck.cs`, `clsDevEventInfo.cs`, `clsDevCommand.cs`에 실제 구현이 있다.
  **문서에서 애매한 부분(ACK 처리, 요청 순서, 필드 고정값)은 이 소스가 정답**

### PC 소스에서 확인한 프로토콜 사실 (2026-09-08)

`/home/jayden/workspace/idti/DeveiceManager/`(Device Manager, 이하 DM)에 장치와 실제로 통신하는 코드가 있다.
**문서보다 이쪽이 정확한 근거**다. 특히 `Source/isldev/`가 프레임·이벤트·상태 구조의 구현체이고,
`Source/IntelliScan Device Manager/`가 그걸 쓰는 응용이다. (소스 주석은 CP949이므로 `iconv -f CP949` 필요)

**1. 접속 방향과 포트** — DM/Platinum 모두 `clsAsynchronousClient.cs`의 `Socket.BeginConnect`로 **장치에
접속하는 클라이언트**다. ACU가 listen 하는 현재 구조가 맞다. 포트 상수는 세 개:

| 상수 | 값 | 의미 |
|------|----|------|
| `defaultPortNumber` | **1004** | PC -> 장치 접속 포트. **우리가 열어야 할 포트** |
| `defaultPortNumberListener` | 1003 | 설정 항목으로만 존재 (DM 소스에 bind/listen 코드 없음) |
| `defaultPortNumberControllerListener` | 1002 | 위와 같음 |

**2. Frame Option 16비트 맵** (`clsDevFrame.cs:BuildFrameOptionByte` + `clsDevCommon.CalcBoolArrayToByte`).
bool 배열 인덱스 0~7이 **뒷 바이트**, 8~15가 **앞 바이트**로 들어가고 바이트 안에서는 LSB부터 채워진다:

| 패킷 위치 | bit | 의미 |
|-----------|-----|------|
| `[4]` (앞) | 7 | **IsRequestAck** |
| `[4]` | 6 | IsPassword |
| `[4]` | 5 | IsDivFrame |
| `[4]` | 4 | IsDataInfo |
| `[4]` | 3 | IsBlocking |
| `[4]` | 2 | IsReRequestAck |
| `[4]` | 1 / 0 | AddrTypeFirst / AddrTypeSecond |
| `[5]` (뒤) | 7 | **IsExcludeDeviceStatus** — 켜지면 응답에서 Device Status를 빼야 함 |
| `[5]` | 6 | **IsTimeSync** (Event Request 시) |
| `[5]` | 5 | **IsReRequestEvent** (Event Request 시) — 직전 이벤트를 다시 달라는 뜻 |
| `[5]` | 4 | **IsCheckSum** — Tail 2byte/4byte를 가르는 비트 |
| `[5]` | 3 / 2 | IsDoorControl / IsEventImage |
| `[5]` | 0 | IsTCP |

-> **우리 구현 검증됨**: `protocol.c`가 응답에 쓰는 `p[4]=0x80`(IsRequestAck)과 요청에서 읽는
`frame_option & 0x0010`(IsCheckSum)이 **둘 다 이 맵과 일치**한다.

**3. Command / Sub / Object 코드** (`isldev/clsDevCommand.cs`)

- Command: `SendAllOver=1, RequestDataRetry=2, SendStatus=3, RequestStatus=4, SendData=5, RequestData=6,
  SendAck=7, RequestAck=8`
- Sub: `IsExist=1, Read=2, Write=3, Delete=4, Change=5, Init=6, RequestBlocking=7, SendBlocking=8`
- Object(일부): `History=1, Action=2, AccessGroup=4, HistoryCount=5, HistoryIndex=6, UserData=33,
  Device=41, Firmware=42, Controller=43, Input=44, Output=45, Module=46, CardReader=47, FPReader=48,
  Timezone=104, Validation=105, Holiday=108, CurrentDateTime=200, OperationMode=201, DoorControl=203,
  UserBinTransStart=208/Continue=209, IOBoardTCPIPConnection=220`

**4. 이벤트 수집 모델이 우리 구현과 다르다** — DM은 이벤트를 이렇게 다룬다:

| 동작 | Command / Sub / Object |
|------|------------------------|
| EventReceive | RequestData(6) / Read(2) / **History(1)** |
| EventCountCheck | RequestData(6) / Read(2) / **HistoryCount(5)** |
| EventIndexChange | SendStatus(3) / Change(5) / **HistoryIndex(6)** |
| EventReset | SendStatus(3) / Init(6) / History 계열 |

즉 장치가 이벤트를 **인덱스로 보관**하고 PC가 "몇 개 있냐 -> 인덱스 이동 -> 받기"로 읽어가는 모델이다.
Event Index 데이터는 `Reserve(1)+Type(1)+Offset(4)+StartDate(6)+EndDate(6)+Reserve(18)`로 날짜 범위 지정도 된다.
**우리는 전송 즉시 큐에서 빼 버리는 모델**이라 `IsReRequestEvent`(직전 것 다시 달라)를 만족시킬 수 없다.
개통 자체는 지금 방식으로도 되지만, 실제 상위 시스템과 맞추려면 인덱스 모델로 바꿔야 한다.

**5. 접속 직후 PC가 처음 보내는 명령** — DM의 상태 조회는 `SettingControllerFirmwareCheck`
= **RequestStatus(4) / Read(2) / Firmware(42=0x2A)** 이다 (`frmNetworkStatus.cs:572`).
장치 시각 확인(`DeviceDateTimeCheck`)도 "프로토콜에 따로 없어서 펌웨어 받기로 확인한다"며 같은 명령을 쓴다.
**현재 acud는 History 읽기 외에는 전부 무시하므로 이 첫 명령에 무응답이다 -> 개통의 1차 관문.**

**6. Device Status V2(234byte) 실제 구조** (`isldev/clsDevStatus.cs`)

```
Category(1) + DeviceType(1) + DateTime(6) + IsExistModule(2)
+ [ ModuleIOType(1) + ModuleIOInstallType(1) + ModuleIOStatus(14) ] x 14모듈 = 224
= 234
```

- 우리 코드는 앞 2byte를 `DeviceType 0x0029`(big-endian)로 쓰는데, 실제로는 **[0]=Category, [1]=DeviceType**이다.
  결과 바이트는 같지만(Category=0x00, Type=0x29) **Category 값이 0이어도 되는지 확인 필요**
- ModuleIOStatus 14byte는 **바이트마다 상위 니블=IO Type, 하위 니블=IO Status**로 읽는다.
  6단계에서 RRU를 IO 모듈로 보고할 때 이 인코딩을 따라야 한다

**7. 기타**

- Password는 `IsPassword` 비트가 켜졌을 때만 의미가 있다 (컨트롤러별 설정)
- 헤더의 Data Block 4필드는 PC 쪽 이름이 `Start / End / Count / OneSize`다 (우리는 Current/End/Total/OneLen)
- Event Info 36byte의 앞 4byte는 우리처럼 하나의 32bit 코드가 아니라
  **Type(1) + Object(1) + Code(1) + Error(1)** 로 나뉜다. 바이트 배치는 같아 호환에 문제 없음

### 통신 안정성 수정 (2026-09-08 완료)

**1. SIGPIPE로 데몬이 통째로 죽던 문제** — `main.c`에 SIGPIPE 무시를 넣고 `net.c`의 send에 `MSG_NOSIGNAL`을
지정. 상위 시스템이 응답을 읽지 않고 연결을 끊으면, 그 소켓에 다음 응답을 쓰는 순간 기본 동작(프로세스 종료)이
걸린다. 수정 전 바이너리로 재현 확인 — 응답을 안 읽고 끊는 클라이언트 **3번째 연결에서 프로세스가 죽음**.
수정 후 같은 테스트를 10회 반복해도 생존하고 카드 판정도 계속 동작함.

**2. 부분 전송(partial send) 처리** — `AcuNet`에 4KB 송신 버퍼를 두고, `send()`가 일부만 보내면 나머지를
남겨 `select()`의 writefds로 이어 보낸다. 기존에는 반환값을 무시해 응답이 조용히 잘릴 수 있었다.
함께 고친 것: **전송에 성공한 뒤에 이벤트를 큐에서 제거**하도록 순서를 바꿔, 전송 실패 시 이벤트가
유실되지 않고 다음 요청 때 다시 나가게 함.

**3. mock HAL을 주입식으로 변경** — 기존 `hal_mock.c`는 `hal_read_card()`가 불릴 때마다(2초 주기) 무조건
카드가 태그된 것처럼 굴어서, PC 통신을 테스트하는 동안 이벤트 큐(32개)가 1분 남짓이면 가득 찼다.
이제 기본은 "아무 일도 없음"이고 FIFO(`acud_mock.fifo`)로 원할 때 주입한다.

```bash
echo 04A1B2C3D4E5F600 > acud/acud_mock.fifo   # 카드 1회 태그 (짧게 넣으면 뒤를 0으로 채움)
echo "door open"      > acud/acud_mock.fifo   # 도어 접점 = 열림 (이벤트의 Door Status에 반영)
echo "door closed"    > acud/acud_mock.fifo
echo "exit on"        > acud/acud_mock.fifo   # Exit 버튼 (main 루프는 아직 미처리)
echo "auto on"        > acud/acud_mock.fifo   # 예전처럼 2초마다 더미 카드 순회
echo "list"           > acud/acud_mock.fifo   # 더미 카드 목록을 로그로 출력
```

FIFO는 `O_RDWR`로 연다 — 읽기 전용으로 열면 쓰는 쪽이 없을 때 read가 계속 EOF를 돌려주기 때문.
여러 줄을 한 번에 넣어도 되도록 카드는 대기 큐(16개)에 쌓아 두고 조회 때마다 한 장씩 꺼낸다.

**4. 테스트 클라이언트를 저장소에 포함** — `tools/idti_client.py`. PC(Platinum)와 같은 역할, 즉 **접속하는
쪽**이다. 5단계 검증 때 쓴 스크립트가 커밋되어 있지 않아 보드에서 다시 검증하려면 매번 새로 만들어야 했다.

```bash
python3 tools/idti_client.py                          # 로컬 1회 조회
python3 tools/idti_client.py --host 192.168.0.50      # 보드에 붙여 조회
python3 tools/idti_client.py --watch --interval 2     # 상위 시스템의 폴링 흉내
python3 tools/idti_client.py --request status         # Device Status 요청 (아직 무응답인 것 확인용)
python3 tools/idti_client.py --raw                    # 주고받은 바이트 그대로 출력
```

**검증 결과**: 이벤트 없을 때 280byte / 이벤트 있을 때 316byte 응답, Event Code·Door Status·Access ID·
BCD 시각 정상 디코딩, 카드 주입 순서대로 이벤트가 하나씩 빠져나감, `door open` 주입이 다음 이벤트의
Door Status에 반영됨을 확인.

### 보드 첫 부팅 / 환경 점검 (2026-09-08)

Radxa 공식 Debian Bullseye b25를 올리고 `tools/board_check.sh`로 점검한 결과.
**여기까지가 오늘 진행분이고, apt 조치·타임존 변경은 아직 실행 전이다** (다음 작업은 TODO "5.5-0" 참고).

| 항목 | 결과 |
|------|------|
| 커널 | `5.10.160-18-rk356x` (Rockchip BSP 5.10) |
| 아키텍처 / OS | `arm64` / Debian 11.8 bullseye |
| 시각 | 정상 (NTP 동작). **타임존은 UTC -> Asia/Seoul로 바꿔야 함** |
| 네트워크 | wlan0 `192.168.0.132` UP / **eth0 DOWN** (제품은 이더넷을 쓸 예정) |
| `cdc_acm` | **있음** (`CONFIG_USB_ACM=m`, `cdc-acm.ko.xz`) -> 6단계 USB 경로 준비 완료 |
| 이미 설치됨 | gcc, make, git, python3 3.9.2, venv, pip3, `libsqlite3.so.0` |
| 없음 | `sqlite3.h`, `cJSON.h` (헤더 2개만 설치하면 빌드 가능) |
| 포트 1004 / 9870 | 둘 다 비어 있음 |
| `apt update` | **실패** — 원인 확정 (아래) |

**`apt update` 실패 원인** — 저장소를 직접 조회해 확인했다.

- **`bullseye-security`의 Release가 만료됨**: `Valid-Until: 2026-09-07 21:13 UTC`.
  Bullseye LTS가 2026-08-31에 끝나 더 갱신되지 않는다. apt는 만료된 Release를 거부한다
- **`bullseye-backports`가 404**: 아카이브에서 제거됨
- `bullseye` main과 `bullseye-updates`는 Valid-Until이 없어 정상이고, radxa 저장소도 정상이다.
  **`deb.debian.org`는 아직 bullseye를 서비스하므로 `archive.debian.org`로 옮길 필요가 없다**
  (오히려 archive에는 `debian-security/bullseye-security`가 없다 — 처음에 세운 가설이 틀렸다)
- 조치: backports 비활성화 + `Acquire::Check-Valid-Until "false"`. 필요한 패키지
  (`libsqlite3-dev 3.34.1-3`, `libcjson-dev 1.7.14-1+deb11u1`)는 bullseye main(arm64)에 있다

점검 스크립트는 `tools/board_check.sh` (읽기 전용). 보드에 복사해 실행하면 위 항목을 한 번에 찍어 준다.

### Firmware(0x2A) 상태 요청 응답 구현 (2026-09-08)

PC가 접속 후 처음 보내는 명령에 응답하도록 했다. **개통의 1차 관문.**

- **요청**: `RequestStatus(0x04) / Read(0x02) / Object=Firmware(0x2A)`
- **응답**: `SendStatus(0x03) / Read(0x02) / Firmware(0x2A)` + Firmware Info 268byte
  - 응답 Command를 `SendStatus(0x03)`로 정한 근거는 Command Table의 Send/Request 짝(3<->4, 5<->6)이다.
    기존 History 응답이 `RequestData(0x06) -> SendData(0x05)`인 것과 같은 방식. **실제 장치가 무엇을
    쓰는지는 PC와 붙여 확인이 필요하다** (PC 소스에서 응답 Command 검증 코드를 못 찾음 — SDK DLL 안에 있는 듯)
- **Firmware Info 268byte** = `Category(1) + DeviceType(1) + Version(4) + DateTime(6, BCD) + Reserved(256)`
  (`isldev/clsDevDeviceSetting.cs`의 `GetFirmwareInfo`). Version 4byte는 PC가 각 byte를 10진수 2자리로
  이어 붙여 표시한다. 현재 값은 1.0.0.0 / 빌드일시 2026-09-08 (`protocol.h`의 `IDTI_FW_*` 상수)
- DM은 장치 시각 확인(`DeviceDateTimeCheck`)에도 같은 명령을 쓴다 — **함께 실리는 Device Status의
  CurDateTime이 PC가 보는 장치 시각**이 되므로 보드 시간 설정이 중요하다

**같이 구현한 것**

- **`IsExcludeDeviceStatus`(Frame Option `[5]` bit7) 처리** — 켜져 있으면 응답에서 Device Status 234byte를
  빼고, 그 비트를 응답 Frame Option에도 실어 PC가 응답 구성을 알 수 있게 했다.
  이를 위해 `idti_build_packet()`에 `frame_option` 인자를 추가함(기존에는 `0x8000` 고정)
- **Device Status의 앞 2byte를 Category/DeviceType으로 분리** — 나가는 바이트는 그대로(0x00, 0x29)지만
  의미를 맞췄다. `IDTI_DEVICE_TYPE_ISC101` -> `IDTI_DEVICE_CATEGORY` + `IDTI_DEVICE_TYPE`
- Frame Option 비트를 `protocol.h`에 상수로 정의 (`IDTI_FOPT_*`) — 기존의 매직넘버 `0x0010` 제거
- 응답 버퍼 512 -> 1024 (`NET_RESP_BUF_CAP`). Firmware 응답이 548byte라 512로는 부족했다

**검증** (`tools/idti_client.py --request status`)

| 요청 | 응답 크기 | 구성 |
|------|-----------|------|
| Firmware | **548byte** | Header 44 + DeviceStatus 234 + Firmware 268 + Tail 2 |
| Firmware + ExcludeDeviceStatus | **314byte** | Header 44 + Firmware 268 + Tail 2 |
| History (이벤트 1건) | 316byte | 기존과 동일 (회귀 없음) |
| History + ExcludeDeviceStatus | 82byte | Header 44 + Event 36 + Tail 2 |
| 미지원 오브젝트(0x2B) | 무응답 | 로그만 남기고 무시 (기존 동작 유지) |

### 보드 개통 성공 (2026-09-09) — acud를 CM3 IO 보드에서 돌리고 PC와 통신 확인

**결과: 개통 1차 성공.** 개발 PC -> 보드 TCP 접속, Firmware 상태 요청 응답, 카드 주입 -> 출입 판정 ->
이벤트가 PC로 올라오는 것까지 전 경로가 실제 하드웨어에서 동작했다.

| 단계 | 결과 |
|------|------|
| 타임존 | `Asia/Seoul` (KST, +0900), NTP 동기화됨 — 로그/이벤트 시각 정상 |
| 의존성 설치 | `libsqlite3-dev 3.34.1-3`, `libcjson-dev 1.7.14-1+deb11u1` (+ `libc6-dev 2.31-13+deb11u11`) |
| 네이티브 빌드 | `gcc -Wall -Wextra -std=c11 -O2` **경고 0개**. `acud` 39,520byte, ELF aarch64 PIE |
| 실행 | 포트 9870 리스닝 확인 (`ss -lntp`) |
| Firmware 상태 요청 | **548byte** 응답 — 개발 PC의 `tools/idti_client.py --request status` |
| 카드 이벤트 | 카드 주입 -> `허용` 판정 -> 릴레이 3초 -> **316byte** 이벤트 응답 |

**배포 방식: `git clone`이 아니라 `rsync`로 확정**

보드에서 `git clone git@github.com:...`은 **실패한다** — 보드에 GitHub에 등록된 SSH 키가 없기 때문이다
(`Permission denied (publickey)`). 배포키를 심는 대신 **개발 PC에서 밀어 넣는 방식**으로 정했다.

```bash
# 개발 PC에서
rsync -av --exclude .git --exclude '__pycache__' ~/workspace/nova-acu/ rock@192.168.0.132:~/nova-acu/
```

보드에 git 자격증명을 심지 않아도 되고, **"제품 ACU에는 소스가 없어야 한다"는 방향과도 어긋나지 않는다.**
보드는 개발 환경이 아니라 "잠깐 빌드하는 곳"이다. 다음 단계에서 이것을 **바이너리만 배포**로 좁힌다
(TODO의 "빌드 방식" 항목 참고).

**apt 문제 — 앞선 분석에서 두 가지가 틀렸다**

2026-09-08에 세운 조치안대로 하면 설치가 **실패한다.** 실제로 해보고 확인한 것:

| 앞선 판단 | 실제 |
|-----------|------|
| "`Check-Valid-Until`을 꺼서 **security 저장소를 살려 두자**" | **틀렸다.** Release는 살아나지만 **풀의 .deb가 삭제돼 있다**(`libc-dev-bin_2.31-13+deb11u14_arm64.deb` -> **404**). bullseye LTS가 2026-08-31에 끝나면서 아카이브에서 빠진 것. 게다가 apt는 모든 패키지의 candidate를 security 버전으로 잡기 때문에 **설치 자체가 통째로 막힌다** |
| "헤더 2개(`sqlite3.h`, `cJSON.h`)만 있으면 된다" | **틀렸다.** `/usr/include/stdio.h`조차 없었다 — **`libc6-dev`가 아예 미설치**였다. gcc는 있어도 아무것도 컴파일 못 하는 상태. `libcjson`은 헤더는 물론 **런타임 `.so`도 없었다** |

**실제로 먹힌 조치** (bullseye main 버전으로 떨어뜨리는 것이 핵심):

```bash
# 1) 사라진 backports 비활성화
sudo mv /etc/apt/sources.list.d/bullseye-backports.list \
        /etc/apt/sources.list.d/bullseye-backports.list.disabled

# 2) 풀이 비어 있는 security 저장소도 비활성화  <- 이것이 핵심
sudo mv /etc/apt/sources.list.d/bullseye-security.list \
        /etc/apt/sources.list.d/bullseye-security.list.disabled

# 3) (있어도 무해) 만료 Release 허용
echo 'Acquire::Check-Valid-Until "false";' \
    | sudo tee /etc/apt/apt.conf.d/99no-check-valid-until

sudo apt update
sudo apt install -y libsqlite3-dev libcjson-dev   # libc6-dev 등이 의존성으로 따라온다
```

security를 끄는 것이 보안상 후퇴로 보이지만, **지금 그 저장소는 설치 가능한 패키지를 하나도 제공하지
못한다**(전부 404). 잃는 것이 실질적으로 없다. 다만 이것은 **bullseye가 EOL이라는 사실을 다시 확인해 준
것**이므로, 제품 출하용 배포판 재선정은 미룰 수 없는 과제다.

**미해결 (개통에는 지장 없음)**

- **radxa 저장소 GPG 키 만료** — `radxa-repo.github.io`의 두 저장소가
  `NO_PUBKEY 67A474DD40402951`, `NO_PUBKEY 5D93177D0752732A`로 서명 검증에 실패해
  `apt update`가 에러를 낸다. 우리 패키지는 전부 Debian main에서 오므로 빌드에는 영향이 없다.
  커널/BSP 패키지를 apt로 갱신해야 할 때 문제가 된다
- **보드 계정이 아직 기본값**(`rock`/`rock`) — 변경 필요

### 5.5-2 완료 (2026-09-09) — 데몬화: 경로 절대화 + 로그 + systemd

손으로 띄우던 프로그램을 **전원만 넣으면 도는 서비스**로 만들었다.

**① 경로 절대화** — 모든 경로가 cwd 상대경로였다. systemd는 cwd를 `/`로 잡기 때문에 그대로는
설정을 못 찾고 DB도 만들지 못한다.

| 경로 | 이전 | 지금 |
|------|------|------|
| config | `config.json` 고정 | **`-c PATH` 옵션** (기본값은 그대로라 개발 흐름은 안 바뀜) |
| DB | config의 `db_path` | 그대로 (절대경로를 넣으면 됨) |
| PID | `acud.pid` 고정 | config의 **`pid_path`** (선택 필드) |
| 로그 | stdout 고정 | config의 **`log_path`** (빈 문자열이면 stdout) |
| mock FIFO | `acud_mock.fifo` 고정 | 환경변수 **`ACU_MOCK_FIFO`** |

- `pid_path`/`log_path`는 **선택 필드**라 기존 config.json이 그대로 동작한다. 키가 없으면 기본값으로
  되돌아가므로, 키를 지우는 것만으로 기본 동작을 되찾을 수 있다
- **mock FIFO만 config가 아니라 환경변수**인 이유: 6단계에서 실제 RRU HAL로 교체되면 사라질 값이라
  제품 설정 스키마(config.json)에 넣지 않았다
- `pid_path`는 실행 중 바뀌어도 따라가지 않는다 — 종료 시 지울 대상이 흔들리면 PID 파일이 남아
  떠돌게 된다. 리로드 시 감지해서 "재시작해야 반영된다"고 로그만 남긴다
- HAL/DB 초기화 실패로 조기 종료할 때도 PID 파일을 지우도록 고쳤다 (죽은 PID가 남으면 웹 설정
  화면이 엉뚱한 프로세스에 SIGHUP을 보낸다)

**② 로그** — `log.c`에 `log_open()/log_reopen()/log_close()` 추가. 기본은 stdout이고,
systemd에서는 stdout이 그대로 journald로 들어가므로 유닛에서는 `log_path`를 비워 둔다.
journald를 안 쓰는 환경을 위해 파일 출력도 되며, **SIGHUP 때 같은 경로로 다시 연다**(logrotate 대응).
로그 파일을 못 열어도 stdout으로 내려가며 데몬은 계속 돈다.

**③ systemd 유닛** (`deploy/acud.service`, `deploy/install.sh`, `deploy/config.json`)

```
/usr/local/sbin/acud            바이너리
/etc/acud/config.json           설정 (0640 root:acud)
/run/acud/{acud.pid,acud_mock.fifo}   RuntimeDirectory - 부팅마다 새로 생성
/var/lib/acud/acud.db           StateDirectory - 재부팅해도 남음
```

- **전용 시스템 계정 `acud`** (로그인 불가). root로 돌리지 않는다
- **`AmbientCapabilities=CAP_NET_BIND_SERVICE`** — 포트 1004는 특권 포트(<1024)라 일반 사용자로는
  bind가 안 된다. root 대신 이 능력 하나만 준다
- `install.sh`는 **바이너리 하나만 있으면 동작한다**(소스 불필요). 5.5-5의 바이너리 전용 배포로
  넘어가도 그대로 쓸 수 있다. 기존 `/etc/acud/config.json`은 **덮어쓰지 않는다**
- mock 카드 주입을 위해 개발 계정을 `acud` 그룹에 넣는다. `UMask=0007`과 함께 FIFO가
  **0660 acud:acud**로 만들어진다 — 누구나 쓸 수 있으면 "출입 허용" 이벤트를 임의로 넣을 수 있다.
  6단계에서 mock이 사라지면 이 설정도 없어진다

**검증 결과 (보드에서 실제 확인)**

| 항목 | 결과 |
|------|------|
| cwd=`/`에서 절대경로만으로 기동 | OK (개발 PC에서 선행 확인) |
| `systemctl enable --now acud` | `active (running)`, `enabled` |
| 로그 -> journald | `journalctl -u acud`로 확인 |
| 런타임 파일 권한 | `/run/acud` 0750 acud:acud, FIFO `prw-rw---- acud acud` |
| **포트 1004 bind (특권 포트)** | **성공** — `acud` 일반 사용자로, 재시작 없이 `systemctl reload`만으로 9870->1004 전환 |
| PC에서 1004 접속 | Firmware 548byte 응답 |
| `kill -9` 후 자동 재시작 | PID 7105 -> 7178, `active` 복귀 |
| 서비스 상태로 카드 주입 -> 이벤트 | 316byte 이벤트 정상 수신 |

**같이 고친 것** — `webui/app.py`가 `ACU_ACUD_DIR` 한 디렉터리에서 config와 PID를 둘 다 찾았는데,
데몬화하면 설정은 `/etc/acud/`, PID는 `/run/acud/`로 흩어진다. `ACU_CONFIG_PATH` / `ACU_PID_PATH`로
따로 지정할 수 있게 했다(기존 방식은 기본값으로 유지).

**남은 것**

- **eth0가 죽어 있다** — `NO-CARRIER`, `/sys/class/net/eth0/carrier = 0`. **케이블 미연결**이다.
  제품은 이더넷을 쓰므로 케이블을 꽂고 다시 봐야 한다. 지금은 wlan0(192.168.0.132)로 통신 중
- **eth0 MAC이 부팅마다 바뀐다** — `0a:96:73:bb:05:8b`는 locally-administered 랜덤 MAC이다
  (RK3566에 MAC이 구워져 있지 않아 커널이 매번 만든다). **DHCP 예약도, PC 쪽 장치 식별도 깨진다.**
  제품에서는 MAC을 고정해야 한다
- 웹UI를 보드에 올릴지는 5.5-5(소스 노출)와 함께 결정

## 빌드 & 실행

```bash
make        # 빌드 -> acud 생성
make run    # 빌드 후 실행
make clean  # 산출물 삭제
```

실행 후 다른 터미널에서:

```bash
kill -HUP  <pid>   # 설정 리로드 신호 (동작 확인용)
kill -TERM <pid>   # 정상 종료
```

또는 실행 중인 터미널에서 `Ctrl+C` (SIGINT) 로도 정상 종료된다.

설정 파일 경로는 `-c`로 바꿀 수 있다 (기본값 `config.json`). `db_path`/`pid_path`/`log_path`는
설정 파일 안에서 지정한다.

```bash
./acud -c /etc/acud/config.json
./acud -h
```

## 보드 배포 (systemd 서비스)

```bash
# 개발 PC -> 보드로 소스 전송
rsync -av --exclude .git --exclude '__pycache__' ~/workspace/nova-acu/ rock@192.168.0.132:~/nova-acu/

# 보드에서
cd ~/nova-acu/acud && make
cd ~/nova-acu && sudo sh deploy/install.sh
sudo systemctl enable --now acud
```

`install.sh`는 **acud 바이너리 하나만 있으면 동작한다**(소스 불필요).
바이너리 경로를 인자로 줄 수 있다: `sudo sh deploy/install.sh /경로/acud`

```bash
systemctl status acud
journalctl -u acud -f              # 로그
sudo systemctl reload acud         # SIGHUP - /etc/acud/config.json 다시 읽기

# mock 카드 주입 (acud 그룹에 속해 있어야 한다. install.sh가 넣어 주며 재로그인 필요)
echo 04A1B2C3D4E5F600 > /run/acud/acud_mock.fifo
```
