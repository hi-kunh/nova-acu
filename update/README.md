# 업데이트 문서함

> NCU(ACU)와 RRU의 **원격 펌웨어 갱신**에 관한 문서.
> 만든 날: 2026-09-14 · DM 쪽은 [`../dm/`](../dm/README.md), RRU 쪽은 [`../rru/`](../rru/README.md)

**기존 IDTi 장비에도 DM에도 없는 기능입니다.** 그래서 상대가 정해져 있지 않고,
DM에 붙일 수도 별도 프로그램으로 만들 수도 있습니다 — 설계 초안 2절이 그 선택지입니다.

## 문서 목록

| 날짜 | 파일 | 내용 | 상태 |
|------|------|------|------|
| 2026-09-14 | [NCU_RRU_펌웨어_업데이트_설계_초안.md](NCU_RRU_펌웨어_업데이트_설계_초안.md) | 전달 경로·`cmd` 규칙·ACU/RRU 절차·안전 장치 | **초안 v0.1 — 결정 8건 대기** |

## 이 초안의 핵심

🟢 **새 프로토콜을 만들 필요가 없습니다.**

```
규약에 범용 파일 전송이 이미 있다   6. IDTi Protocol FileBinaryTransmit.doc
  Send Start     Cmd 5 / Sub 3 / Obj 0xDD   총 크기 + cmd 길이 + cmd ASCII
  Send Continue  Cmd 5 / Sub 3 / Obj 0xDE   Index + 원시 데이터

결과 이벤트 코드도 이미 있다       event_codes_all.csv
  18010506 Firmware Update Success   /   18010507 Firmware Update Failed
```

`cmd ASCII`가 자유 문자열이라 **"이 파일이 무엇인가"를 거기 적으면 됩니다.**
그래서 **DM이 보내든 별도 프로그램이 보내든 ACU는 구분하지 않습니다** — 같은 프레임입니다.
DM 개발이 늦어도 별도 프로그램으로 먼저 쓸 수 있고, 나중에 DM이 붙어도 ACU는 그대로입니다.
