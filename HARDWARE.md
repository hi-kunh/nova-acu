# 하드웨어 구성

이 프로젝트의 물리적 장치 구성 기록. 소프트웨어 진행 상황은 [README.md](README.md),
남은 작업은 [TODO.md](TODO.md) 참고.

- 최종 갱신: 2026-09-07
- 상태: **RRU 보드는 설계 단계** (실물 확보 전). 아래 채널 번호 체계는 확정 전 제안값

---

## 전체 구성

```
  [상위 시스템 / 웹앱]
          |
          |  Ethernet - TCP / IDTi 프로토콜 V2      (5단계: 구현 완료)
          |
  +-------------------------------------------+
  |  ACU  (Access Control Unit)               |
  |  RK3566 / Radxa CM3 IO Board              |   <- 이 저장소의 acud 데몬
  |  임베디드 리눅스, headless                 |
  +-------------------------------------------+
          |
          |  UART - ACU<->RRU 프로토콜             (6단계: 신규 설계·구현 대상)
          |
  +-------------------------------------------+
  |  RRU  (Remote Reader Unit)                |
  |  리더/센서/릴레이 물리 I/O 담당             |
  +-------------------------------------------+
     |        |         |
     |        |         +-- 출력 8채널  (릴레이, 채널별 NO/NC)
     |        +------------ 입력 24채널 (Exit / Door / Lock) x 8
     +--------------------- 리더 8채널  (Wiegand)
```

**핵심**: 리더기·센서·릴레이는 ACU(RK3566)에 **직접 연결되지 않는다.**
Wiegand 수신과 GPIO 제어는 전부 RRU가 담당하고, ACU는 UART로 RRU와만 통신한다.
즉 ACU 쪽 6단계 작업은 "RK3566에서 Wiegand를 직접 받는 일"이 아니라
**"ACU-RRU 간 UART 프로토콜을 설계하고 구현하는 일"** 이다.

---

## ACU 보드

| 항목 | 내용 |
|------|------|
| SoC | RK3566 |
| 보드 | Radxa CM3 IO Board (보유 중) |
| OS | 임베디드 리눅스 (headless CLI 데몬) |
| 상위 통신 | Ethernet / TCP, IDTi 프로토콜 V2 |
| 하위 통신 | UART -> RRU |
| 역할 | 출입 판정, DB(SQLite3), 설정 관리, 상위 시스템 연동, 이벤트 로그 |

### RK3568 / CAN 경로 (이번 범위 제외)

RK3568 기반 구성에서는 하위 통신을 UART 대신 **CAN bus**로 가져갈 예정이나,
보드 미확보 상태이며 현재 범위에서는 진행하지 않는다.
다만 HAL 아래에 전송 계층(UART/CAN)을 갈아끼울 수 있는 구조로 만들어 두면
나중에 프로토콜 상위 계층은 재사용할 수 있다.

---

## RRU 보드 (Remote Reader Unit)

| 구분 | 수량 | 비고 |
|------|------|------|
| 리더 (Wiegand) | 8 | 리더 1개 = 도어 1개 기준 |
| 입력 (Input / Sensor) | 24 = 3 x 8 | 도어당 Exit, Door, Lock 3개 |
| 출력 (Output / Relay) | 8 | 도어당 1개, 채널별 NO/NC 선택 |

### 채널 구성표 (번호 체계 제안 - 확정 필요)

도어 1개당 리더 1 + 입력 3 + 출력 1이 한 묶음으로 대응된다는 전제.

| 도어 | 리더 | 입력 (Exit) | 입력 (Door) | 입력 (Lock) | 출력 (Relay) |
|:----:|:----:|:-----------:|:-----------:|:-----------:|:------------:|
| 1 | R1 | IN1  | IN2  | IN3  | OUT1 |
| 2 | R2 | IN4  | IN5  | IN6  | OUT2 |
| 3 | R3 | IN7  | IN8  | IN9  | OUT3 |
| 4 | R4 | IN10 | IN11 | IN12 | OUT4 |
| 5 | R5 | IN13 | IN14 | IN15 | OUT5 |
| 6 | R6 | IN16 | IN17 | IN18 | OUT6 |
| 7 | R7 | IN19 | IN20 | IN21 | OUT7 |
| 8 | R8 | IN22 | IN23 | IN24 | OUT8 |

### 입출력 채널 의미

- **Exit** — 퇴실/개방 버튼. 눌리면 판정 없이 해당 도어 릴레이 동작
- **Door** — 도어 접점(Door Contact). 문이 실제로 열렸는지/닫혔는지
- **Lock** — 잠금 상태(Lock State). 락이 실제로 걸렸는지
- **Relay(출력)** — 도어 릴레이. 인증 성공 시 설정된 시간(초)만큼 동작.
  채널별 **NO(Normal Open) / NC(Normal Close)** 선택 가능 -> 설정 항목으로 노출 필요

---

## IDTi 프로토콜 매핑

기존 상위 시스템과 호환되어야 하므로, 위 구성을 IDTi 규격에 아래와 같이 대응시킨다.

| RRU 구성 | IDTi 대응 | 근거 문서 |
|----------|-----------|-----------|
| 리더 8채널 | Reader Device Type `0x8d~0x91` | `7. Device Type Table` |
| 입력 Exit | `Door Control(Exit Button) 0xae` | `7. Device Type Table` |
| 입력 Door | `Door Contact 0xac` | `7. Device Type Table` |
| 입력 Lock | `Lock State 0xad` | `7. Device Type Table` |
| 입력 24채널 전체 | Device Input(Sensor) Object `0x2C`, Index 1~254 | `9. Relay & Sensor` |
| 출력 8채널 | Device Output(Relay) Object `0x2D`, `ActiveType=1(Door Relay)` | `9. Relay & Sensor` |
| 출력 NO/NC | Sensor 쪽 `ActiveType`(0=NC, 1=NO)과 같은 개념 | `9. Relay & Sensor` |

**입력 3종이 IDTi 센서 3종과 정확히 일치**한다는 점이 중요하다
(Exit/Door/Lock = `0xae`/`0xac`/`0xad`). 별도 변환 없이 그대로 매핑된다.

채널 Index 범위가 1~254이므로 8채널/24채널은 여유 있게 들어간다.

### 프로토콜 버전

**V2 유지** (Device Status 234byte). V2는 최대 28리더를 지원하므로 8리더는 충분히 들어간다.
5단계에 적어둔 선택 근거("단일 도어/단일 리더 장치에는 V3가 과함")는 구성 변경으로 더 이상
맞지 않으니, **"8리더는 V2 범위(28) 안에 들어감"** 으로 근거를 바꿔 읽을 것.

---

## 미확정 사항 (6단계 착수 전 결정 필요)

- [ ] **ACU 1대에 RRU를 몇 대 붙일 것인가** — 1대 고정인지, UART 멀티드롭(RS-485)으로 여러 대인지.
      프로토콜에 장치 주소 필드가 필요한지가 여기서 갈린다
- [ ] **UART 물리 규격** — RS-232 / RS-485(TTL?), baud rate, 프레이밍, 배선 길이
- [ ] **RK3566에서 쓸 UART 포트** — Radxa CM3 IO Board의 어느 UART를 쓸지, 디바이스 노드 경로
- [ ] **ACU-RRU 프로토콜 설계** — 프레임 구조, 카드 이벤트 통지 방식(폴링 vs RRU가 먼저 보냄),
      릴레이 제어 명령, 센서 상태 조회, CRC, 재전송/타임아웃, RRU 연결 끊김 감지
- [ ] **Wiegand 카드 ID 형식** — 26bit(facility 8 + card 16) / 34bit 중 무엇을 쓸지, 그리고 이것을
      현재 DB 스키마의 `card_id`(hex 16자) 형식에 어떻게 담을지
- [ ] **채널 번호 체계 확정** — 위 표는 제안값. RRU 펌웨어 쪽 번호와 일치시켜야 함
- [ ] **RRU 미연결/장애 시 ACU 동작** — 로그만 남기고 계속 동작할지, 상위 시스템에 장애 이벤트를 올릴지
- [ ] **장치 타입 코드 확정** — RRU가 IDTi Remote Module(RIM/ROM/RRM/RXM) 중 무엇에 대응하는지,
      Device Status의 `ExistedModule`/`IOModuleStatus`로 RRU를 보고해야 하는지
      (현재 코드는 전부 0으로 채우고 있음. 미확인 문서 `8. System Device Reader Setup` 확인 대상)
