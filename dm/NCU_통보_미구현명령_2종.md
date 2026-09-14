# NCU 통보 — 오늘 실측에서 실패한 명령 2종 · 그리고 좋은 소식

> 보내는 쪽: IntelliScan Device Manager(서버) / Platinum 쪽
> 작성: 2026-09-14 · 현장 실측 기록(`userlogtbl`) 근거
> 판단 기준은 그대로 **기존 IDTi 장비(SSC-324) 대체**입니다.

---

## 1. 🎉 먼저 좋은 소식 — **ncu01 이 SSC-324 와 숫자가 같아졌습니다**

09-14 09:52 에 장치구조를 다시 받았고, 결과가 **질의 6-2 · 6-3 의 답**입니다.

```
devmoduleenabled        11000000000000     ← IsExistModule `00 03` 이 그대로 들어왔습니다 🟢
devmodule1·2iocategory  22333333444433     ← SSC-324 와 한 글자도 다르지 않습니다
mType / installedtype   61 / 1             ← 🟢 SSC-324 값으로 맞추셨습니다 (이전 123 / 2)
포인트                  리더 4 · 입력 16 · 출력 8 · 입출력명칭 28행
commstatus              0 (정상)
```

**현장 SSC-324(ACU03·ACU04)와 완전히 같은 숫자입니다.**

| 질의 | 결과 |
|---|---|
| **6-2** `IsExistModule` `00 03` 이 서버 표에서 `11000000000000` 이 되는가 | 🟢 **그렇습니다.** 바이트 순서 해석도 맞았습니다 |
| **6-3** SSC-324 의 `mType` | 🟢 **61 / installedtype 1** 로 맞추신 것이 확인됐습니다 |

그리고 **입력 설정 쓰기 7건이 전부 성공**했습니다.

```
09:53:15  [5:1:13]                                    성공
09:53:33~57  [5:2:3] [5:2:4] [5:2:5] [5:2:6] [5:2:7] [5:2:8]   전부 성공
```

⇒ `SettingInputValueChange`(Command 5 / Sub 5 / Object 44) 는 **정상 동작합니다.**

---

## 2. 🔴 실패한 명령 2종

Platinum 의 **「장치 정보 업데이트」** 는 `*Check` 계열을 한꺼번에 보냅니다.
현장 SSC-324 두 대는 모두 성공했는데 `ncu01` 만 실패한 것이 둘입니다.

### 2-1. `SettingControllerBasisValueCheck` — **컨트롤러 기본설정 받기**

```
09:52:30  ncu01  실패
10:01:31  ncu01  실패          ← 장치구조를 받은 뒤에도 실패

같은 시각 다른 장비
10:06:32  ACU04  성공  →  응답값 "1/5/0"
10:06:47  ACU03  성공  →  응답값 "1/5/0"
10:05:22  ACAM01 성공  →  빈 응답 (ACAM 은 서버가 대신 답합니다. 정상)
```

**프레임:**

```
Command      = 6  (RequestData)
CommandSub   = 2  (Read)
Object       = 41 (Device)
```

**응답에 담을 것** (`isldev\clsDevControllerBasisInfo.cs`):

| 필드 | ACU03·ACU04 실측값 |
|---|---|
| `ControllerBasisCategory` | (표시되는 세 값이 `1/5/0` 입니다 — 아래 참고) |
| `ControllerBasisType` | |
| `ControllerBasisAddress` | |
| `ControllerBasisOperationMode` | |
| `ControllerBasisLevel` | |
| `ControllerBasisValidation` | |
| `ControllerBasisFloor` | |

🟡 Platinum 로그에 찍히는 `1/5/0` 은 이 중 **세 개만 추린 표시값**입니다.
전체 필드의 원시 바이트가 필요하시면 **ACU03·ACU04 로 캡처해서 보내드리겠습니다** — 요청 주십시오.

### 2-2. `SettingInputValueCheck` — **입력센서 설정 받기(되읽기)**

```
09:54:43  ncu01  [5:1:3]  실패
```

**쓰기는 되는데 되읽기가 안 됩니다.** 같은 Object(44) 의 Read 방향입니다.

```
쓰기  SettingInputValueChange   Command = 5 (SendData)     Sub = 5 (Change)   Object = 44   🟢 성공
읽기  SettingInputValueCheck    Command = 6 (RequestData)  Sub = 2 (Read)     Object = 44   🔴 실패
```

응답 데이터는 **쓰기와 같은 21바이트 배치**를 그대로 돌려주시면 됩니다
(배치표는 앞선 회신 `NCU_DM_질의_RRU구성_이벤트보관_회신.md` 5-1 절에 있습니다).

---

## 3. 왜 이 둘이 중요한가

**Platinum 의 「장치 정보 업데이트」 한 번에 `*Check` 계열이 통째로 나갑니다.**
하나라도 실패하면 **그 화면 전체가 실패로 보입니다** — 운영자는 "장비가 이상하다" 고 읽습니다.

대체 목적에서는 **현장 SSC-324 가 답하는 명령은 NCU 도 답해야** 합니다.
지금 확인된 것만 옮기면 이렇습니다:

```
🟢 이미 되는 것   SettingControllerFirmwareCheck        (펌웨어 조회)
                  장치구조 다운로드
                  SettingInputValueChange               (입력 설정 쓰기)
🔴 안 되는 것     SettingControllerBasisValueCheck      (컨트롤러 기본설정 받기)
                  SettingInputValueCheck                (입력센서 설정 받기)
```

🟡 **오늘 C부류(조회 계열) 전수 점검을 진행 중입니다.** 끝나면 *"현장 SSC-324 가 답하는 명령 전체 목록"*
을 정리해서 보내드리겠습니다 — **그것이 NCU 가 구현해야 할 조회 명령의 확정 목록**이 됩니다.

---

## 4. 회신 부탁드릴 것

| | 내용 |
|---|---|
| 1 | 위 2종을 구현 일정에 넣어 주실 수 있는지 |
| 2 | `ControllerBasisInfo` 전체 필드의 **원시 바이트 캡처**가 필요하신지 (필요하면 ACU03·04 로 떠 드립니다) |
| 3 | 그 밖에 *"이 명령은 어떻게 답해야 하나"* 궁금한 것 — 실장비로 재서 답하겠습니다 |

---

# 🔴 추가 통보 (2026-09-14 11:00) — **조회 명령 전수 검증 결과**

Platinum 「장치 정보 업데이트」로 조회 계열 **12종**을 6대에 한꺼번에 돌렸습니다(총 354건).
앞서 "C부류가 끝나면 확정 목록을 보내겠다"고 한 그 결과입니다.

## 결과

```
🟢 ACAM 3대     12종 전부 성공 (실패 0건)
🟢 SSC-324 2대  11종 성공.  엑세스그룹 적용 받기만 실패 — 장비에 그 데이터가 없어서로 판단
🔴 ncu01        12종 중 **펌웨어 확인 1종만 성공**
```

## 🔴 NCU 가 구현해야 할 조회 명령 — 확정 목록

**현장 SSC-324 가 답하는 것은 NCU 도 답해야 합니다.**

| 명령 | 프레임 | 비고 |
|---|---|---|
| `SettingControllerBasisValueCheck` | Cmd 6 / Sub 2 / **Obj 41** | 컨트롤러 기본설정 |
| `SettingControllerValueCheck` | Cmd 6 / Sub 2 / **Obj 43** | 컨트롤러 장치설정 |
| `SettingInputValueCheck` | Cmd 6 / Sub 2 / **Obj 44** | 입력센서 — **쓰기(Sub 5)는 이미 성공합니다** |
| `SettingOutputValueCheck` | Cmd 6 / Sub 2 / **Obj 45** | 출력릴레이 |
| `SettingCardReaderValueCheck` | Cmd 6 / Sub 2 | 카드리더 |
| 알람 설정 받기 | Cmd 6 / Sub 2 | |
| 경보벨 스케줄 받기 | Cmd 6 / Sub 2 | |
| 운전모드 스케줄 받기 | Cmd 6 / Sub 2 | |
| 도어모드 스케줄 받기 | Cmd 6 / Sub 2 | |
| 비상도어모드 받기 | Cmd 6 / Sub 2 | |
| 엑세스그룹 적용 받기 | Cmd 6 / Sub 2 | SSC-324 도 실패 중이라 **판정 보류** |

🔴 **패턴이 또렷합니다 — `Command 6(RequestData) / Sub 2(Read)` 방향이 통째로 비어 있습니다.**
쓰기(`Command 5 / Sub 5`)는 동작합니다.

## 명령별 반복 횟수 — 포인트 수만큼 나갑니다

```
카드리더 받기     리더 수만큼      (SSC-324 구성이면 4회)
입력센서 받기     입력 수만큼      (16회)
출력릴레이 받기   출력 수만큼      (8회)
컨트롤러 계열     1회
```

🟢 참고로 **ncu01 의 실행 건수가 SSC-324 와 정확히 같았습니다**(리더 4 · 입력 16 · 출력 8).
구조를 맞추신 것이 서버에서 그대로 확인됩니다.

## 원시 바이트가 필요하시면

응답 형식을 맞추실 때 **현장 SSC-324 의 실제 응답**이 가장 확실합니다.
**어느 명령이든 말씀해 주시면 ACU03·ACU04 로 캡처해서 보내드리겠습니다.**
