#ifndef ACU_USERBIN_H
#define ACU_USERBIN_H

#include <stddef.h>
#include <stdint.h>

#include "users.h"

/*
 * 사용자 바이너리 전송 수신 (`5. IDTi Protocol UserBinaryTransmit.doc`).
 *
 * DM이 명단 전체를 내려보내는 경로다. 프레임 하나에 다 담을 수 없어
 * **총 크기를 먼저 알려 주고(Start), 조각을 번호와 함께 이어서 보낸다(Continue).**
 *
 *   Start     Cmd 5 / Sub 3 / Obj 0xD0   총 크기(4) + 총 인원(4) + OBJ(1) + Rev(1)
 *   Continue  Cmd 5 / Sub 3 / Obj 0xD1   Index(2) + 원시 데이터 N
 *
 * 조각은 **레코드 경계와 무관하게** 잘려 온다(N은 1664·3616·7232·14464·28928·57856 중 하나).
 * 그래서 여기서는 바이트 흐름으로 받아 레코드 크기(128 또는 1808)가 찰 때마다 한 명씩 해석한다.
 * 명단 전체를 메모리에 쌓지 않는다 — 26만 명이면 33MB다.
 *
 * 받은 사용자는 `users.h`의 **전체 다운로드 경로**(새 표에 받아 한 번에 교체)로 들어간다.
 * 그래서 받는 동안에도 기존 명단으로 출입 판정이 계속되고, 중간에 끊기면 원래 명단이 남는다.
 */

typedef struct AcuUserBin AcuUserBin;

/* Continue 처리 결과 */
typedef enum {
    USERBIN_OK = 0,      /* 잘 받았다. 더 올 것이 있다 */
    USERBIN_COMPLETE,    /* 총 크기만큼 다 받아 명단을 교체했다 */
    USERBIN_FAILED,      /* 받지 못했다 — PC에 Fail로 답하면 직전 조각을 다시 보낸다 */
} AcuUserBinStatus;

/* users는 저장소. 실패 시 NULL */
AcuUserBin *userbin_create(AcuUsers *users);
void userbin_destroy(AcuUserBin *ub);

/*
 * Start 수신. 이미 받는 중이었다면 그것을 버리고 새로 시작한다.
 * total_size: 명단 바이너리 전체 byte 수, total_count: 사용자 수
 * binary_obj: IDTI_USERBIN_OBJ_* (레코드 크기가 여기서 정해진다)
 * 반환: 0=성공(Success로 답한다), -1=실패
 */
int userbin_start(AcuUserBin *ub, uint32_t total_size, uint32_t total_count, uint8_t binary_obj);

/* Continue 수신. 반환은 AcuUserBinStatus */
AcuUserBinStatus userbin_continue(AcuUserBin *ub, uint16_t index,
                                  const uint8_t *data, size_t len);

/*
 * 다 받았을 때(USERBIN_COMPLETE) 올릴 이벤트 코드를 돌려준다
 * (IDTI_EVENT_USERFILE_SUCCESS / _PARTIAL / _FAIL).
 */
uint32_t userbin_result_event(const AcuUserBin *ub);

/* 받는 중인지 (1=받는 중) */
int userbin_in_progress(const AcuUserBin *ub);

/*
 * 받다 만 것을 버린다. 상위 시스템 연결이 끊기면 불러야 한다 —
 * 안 그러면 받다 만 표가 남고, 다음 Start가 그것을 이어받은 것으로 오해할 수 있다.
 */
void userbin_abort(AcuUserBin *ub);

/*
 * ---- 규약 레코드 ↔ 저장 구조체 변환 ----
 *
 * User Info 32byte는 **바이너리 전송과 1명씩 경로가 같은 것을 쓴다**(`3.` 문서 A절).
 * 바이너리 쪽에서 먼저 필요해 여기 두었고, 1명씩 경로(usercmd.c)도 같은 함수를 쓴다.
 */
void userbin_parse_user_info(const uint8_t *info, AcuUserRecord *r);
void userbin_build_user_info(const AcuUserRecord *r, uint8_t *info);

/* 버퍼가 전부 0인지 (카드가 없는 사용자 등을 가려낼 때) */
int userbin_all_zero(const uint8_t *p, size_t n);

#endif /* ACU_USERBIN_H */
