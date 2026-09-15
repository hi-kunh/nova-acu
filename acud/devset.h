#ifndef ACU_DEVSET_H
#define ACU_DEVSET_H

#include <stddef.h>
#include <stdint.h>
#include <sqlite3.h>

/*
 * 장치 설정 저장 (`acud.db`의 `device_settings` 표).
 *
 * DM이 내려보내는 입력·출력·컨트롤러 설정을 **받은 byte 그대로(BLOB)** 보관한다.
 * 필드로 풀어 저장하지 않는 이유: 조회(`Cmd 6 / Sub 2`)가 오면 **보낸 값을 그대로** 돌려줘야 한다
 * (DM 검증보고서 5-1: 기존 SSC-324는 되읽기가 쓴 값과 한 글자도 다르지 않다).
 * 풀었다 다시 묶으면 우리가 모르는 예약 칸이 틀어질 수 있다. 판단에 필요한 칸(출력의
 * ActiveType 등)은 읽을 때 오프셋으로 꺼낸다.
 *
 * 열쇠는 (오브젝트, 모듈, 칸)이다. 칸은 **모듈 14칸 배치의 자리 번호(0~13)** 다
 * (`22333333444433` — 0·1 리더 / 2~7 입력 / 8~11 출력 / 12·13 입력(알람·화재)).
 * 컨트롤러 기본설정처럼 모듈이 없는 것은 (오브젝트, 0, 0)이다.
 *
 * 사용자 명단(`users.db`)·이벤트(`events.db`)와 달리 이 표는 작다(모듈 14 × 14칸 × 몇 종).
 * 그래서 설정 파일인 acud.db에 둔다.
 */

/*
 * 저장된 설정 블록을 꺼낸다. out_len은 기대하는 블록 크기다.
 * 반환: 1=찾음, 0=저장된 것 없음, -1=오류(크기가 다르거나 DB 오류)
 */
int devset_get(sqlite3 *db, uint8_t object, int module, int slot, uint8_t *out, size_t out_len);

/* 설정 블록을 저장한다 (있으면 바꾼다). 반환: 0=성공, -1=실패 */
int devset_put(sqlite3 *db, uint8_t object, int module, int slot, const uint8_t *data, size_t len);

/* 표가 없으면 만든다 (db_open 뒤 한 번). 반환: 0=성공, -1=실패 */
int devset_init(sqlite3 *db);

#endif /* ACU_DEVSET_H */
