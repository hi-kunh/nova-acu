# ACU (Access Control Unit)

임베디드 리눅스 기반 출입보안 장치. 화면 없는(headless) CLI 데몬으로 동작한다.
기존 STM32 기반 제품(IDTi 시리즈)과 동일한 통신 프로토콜을 사용하여, RK3566/RK3568 기반으로 재구현하는 프로젝트.

## 개발 단계

- **[0단계] 데몬 뼈대**  완료 - `acud` 폴더
- **[1단계] SQLite3 + 출입 판정 로직**  완료 (아래 "1단계 완료 기록" 참고)
- 2단계: 하드웨어 추상화 계층(HAL) 인터페이스 확정
- 3단계: config.json 감시 + 무중단 리로드
- 4단계: 웹 설정 인터페이스 (Flask)
- 5단계: 네트워크 통신부 (TCP/UDP)
- 6단계: 실제 하드웨어 (GPIO / Wiegand : rk3566 시리얼 통신, rk3568 can bus)

## 대상 하드웨어

- 기존 제품: STM32 기반, IDTi 프로토콜로 통신
- 신규 목표: RK3566 또는 RK3568
- **현재 보유 보드**: RK3566 기반 **Radxa CM3 IO Board**
- 최종적으로 기존 제품과 100% 동일한 IDTi 프로토콜로 통신 가능하게 만드는 것이 목표 (기존 상위 시스템/웹앱과 호환)

## 참고 프로토콜 문서 (기존 장치 통신 규격)

경로: `/home/jayden/workspace/idti/IDTI WebApp Protocol/`

이 프로젝트가 반드시 호환해야 하는 기존 IDTi 프로토콜 문서 모음. 특히 아래 두 문서를 우선 확인함:

- `1. IDTi Protocol V1_2_3 Basic structure.doc` — 패킷 프레임 기본 구조
- `5. IDTi Protocol UserBinaryTransmit.doc` — 유저 DB 대량 전송(바이너리) 프로토콜

그 외 폴더 내 문서(향후 단계에서 필요할 때 참고):
`2. Event Structure & Event Code`, `3. Member(User DB) Structure`, `4. TimeZone & Holiday & Validation`,
`6. FileBinaryTransmit`, `7. Device Type Table`, `8. System Device Reader Setup`,
`9. Relay(Output) & Sensor(Inout) Function`, `10. Group`, `11. User General Group`, `12. Canteen`,
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
