# 작업 목록 (TODO)

ACU 프로젝트의 진행 상황과 남은 일 정리.
완료된 단계의 **상세 기록은 [README.md](README.md)** 에 있고, 이 파일은 "무엇이 남았는지"만 관리한다.

- 최종 갱신: 2026-09-04
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
| 6 | 실제 하드웨어 (GPIO / Wiegand / CAN) | ⬜ 다음 작업 |
| 7+ | 배포/운영 (systemd, 보안 강화 등) | ⬜ 미착수 |

범례: ✅ 완료 · 🔶 부분 완료(남은 항목 아래 참고) · ⬜ 미착수

---

## 다음 작업: 6단계 — 실제 하드웨어 연동

`hal_mock.c`를 실제 구현체로 교체하는 단계. `hal.h` 인터페이스는 2단계에서 확정해 두었으므로
`main.c` / `access.c` 는 건드리지 않는 것이 목표.

- [ ] Radxa CM3 IO Board(RK3566) 핀맵 확인 — 리더기 D0/D1, 도어 릴레이, 도어 접점, 퇴실 버튼에 쓸 핀 결정
- [ ] `hal_rk3566.c` 작성 — Wiegand 26/34bit 수신 (시리얼 통신)
  - [ ] D0/D1 falling edge 인터럽트 수신 (gpiod 또는 sysfs 방식 결정 필요)
  - [ ] 비트 타임아웃 처리 + 패리티 검증 → 카드 ID 변환
  - [ ] `hal_read_card()` 를 폴링 방식 그대로 유지할지, 이벤트 큐 방식으로 바꿀지 결정
- [ ] 도어 릴레이 출력 GPIO 제어 — `hal_open_door(seconds)` 구현 (블로킹 회피: 별도 스레드 또는 만료 시각 관리)
- [ ] 센서 입력 GPIO 읽기 — `hal_read_sensor()` (Door Contact / Exit Button, Normal Open/Close 설정 반영)
- [ ] RK3568 CAN bus 경로 검토 (보드 확보 후) — `hal_rk3568.c`
- [ ] Makefile에 HAL 구현체 선택 스위치 추가 (`HAL=mock|rk3566|rk3568`)
- [ ] 실제 카드로 판정 → 릴레이 동작까지 엔드투엔드 확인
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

- [ ] `hal_read_card()` 폴링 주기 2초가 실제 리더기에 맞는지 재검토 (6단계에서 결정)
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
