#ifndef ACU_USERCMD_H
#define ACU_USERCMD_H

#include <stddef.h>
#include <stdint.h>

#include "users.h"

/*
 * 사용자 1명씩 주고받는 명령 (`1. IDTi Protocol Basic structure`).
 *
 * DM은 전체 명단을 바이너리로 한 번에 내려보내고(`userbin.h`), **개별 추가·수정·삭제·조회는
 * 이 명령들로 한다.** 둘 다 구현이 필요하다 (2026-09-11 DM 회신 7-4).
 *
 *   전송  Cmd 5 / Sub 3 / Obj 사용자 오브젝트   Data(N)  -> Result(1)
 *   삭제  Cmd 5 / Sub 4 / Obj 사용자 오브젝트   Data(12) -> Result(1)
 *   받기  Cmd 6 / Sub 2 / Obj 사용자 오브젝트   Data(12) -> 그 오브젝트의 데이터
 *
 * Data(12)는 User ID(8) + Revision ID(4)다. 오브젝트 종류(0x15/0x16/0x17/0x21 …)는
 * DM이 무엇을 담아 보내느냐로 정해지므로, 받을 때는 **길이로 무엇이 왔는지 가린다.**
 */

/* 처리 결과 — 프레임의 Result 한 byte로 나간다 */
typedef enum {
    USERCMD_OK = 0,
    USERCMD_FAIL,
} AcuUserCmdStatus;

/*
 * 사용자 전송(Set). data는 오브젝트에 따라 길이가 다르다.
 * 지금 받아 저장하는 것: Info(32) 단독 / Info+Name(48) / UserData(112).
 * 지문·사진이 붙은 오브젝트는 앞부분만 읽고 나머지는 버린다 (현장 IDTi는 지문을 쓰지 않는다).
 */
AcuUserCmdStatus usercmd_set(AcuUsers *users, uint8_t object,
                             const uint8_t *data, size_t len);

/* 사용자 삭제. data는 User ID(8) + Revision(4) */
AcuUserCmdStatus usercmd_delete(AcuUsers *users, const uint8_t *data, size_t len);

/*
 * 사용자 받기. data는 User ID(8) + Revision(4).
 * 찾으면 out에 그 오브젝트의 데이터를 만들고 길이를 *out_len에 넣는다.
 * 못 찾으면 out_len=0으로 두고 USERCMD_FAIL을 돌려준다.
 * out은 최소 IDTI_USERDATA_LEN byte여야 한다.
 */
AcuUserCmdStatus usercmd_get(AcuUsers *users, uint8_t object,
                            const uint8_t *data, size_t len,
                            uint8_t *out, size_t *out_len);

/* 이 오브젝트가 사용자 명령인지 (net.c가 분기할 때 쓴다) */
int usercmd_is_user_object(uint8_t object);

#endif /* ACU_USERCMD_H */
