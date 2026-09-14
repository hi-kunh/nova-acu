#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>

#include "usercmd.h"
#include "userbin.h"
#include "protocol.h"
#include "log.h"

int usercmd_is_user_object(uint8_t object)
{
    switch (object)
    {
        case IDTI_OBJ_USER_ALL:
        case IDTI_OBJ_USER_INFO:
        case IDTI_OBJ_USER_CARD:
        case IDTI_OBJ_USER_FINGER:
        case IDTI_OBJ_USER_RESTRICT:
        case IDTI_OBJ_USER_DATA:
        case IDTI_OBJ_USER_DATA_FGR:
        case IDTI_OBJ_USER_IMAGE:
            return 1;
        default:
            return 0;
    }
}

/*
 * 카드 32byte(Proximity Data)에서 판정에 쓰는 카드값 8byte를 꺼낸다.
 *
 * **이 경로의 앞 8byte는 뒤집혀 있다** — `1.` 문서의 Card ID 예시에 "(Reversed Bytes)"라고
 * 적혀 있고(`0xc1/0x5d/0x06/0x02` 로 실린 것이 실제로는 `02/06/5d/c1`),
 * `clsDevUserBin.cs`도 바이너리 쪽 값과 이 값이 서로 뒤집힌 관계임을 보여 준다.
 * 되돌리면 바이너리 전송·이벤트 Access ID와 같은 "큰 자리부터" 표현이 된다.
 */
static void prox_to_card_id(const uint8_t *prox32, uint8_t card_id[ACU_USER_CARD_LEN])
{
    for (int i = 0; i < ACU_USER_CARD_LEN; i++)
    {
        card_id[i] = prox32[ACU_USER_CARD_LEN - 1 - i];
    }
}

/* 위의 반대 — 저장해 둔 카드값을 되돌려 줄 때 쓴다 */
static void card_id_to_prox(const uint8_t card_id[ACU_USER_CARD_LEN], uint8_t *prox32)
{
    memset(prox32, 0, IDTI_USERDATA_LEN_CARD);
    for (int i = 0; i < ACU_USER_CARD_LEN; i++)
    {
        prox32[i] = card_id[ACU_USER_CARD_LEN - 1 - i];
    }
}

AcuUserCmdStatus usercmd_set(AcuUsers *users, uint8_t object,
                             const uint8_t *data, size_t len)
{
    if (!users || !data || len < IDTI_USERINFO_LEN)
    {
        log_msg("사용자 명령: 전송 데이터가 User Info보다 짧다");
        return USERCMD_FAIL;
    }

    AcuUserRecord r;
    memset(&r, 0, sizeof(r));
    userbin_parse_user_info(data, &r);

    if (userbin_all_zero(r.user_id, ACU_USER_ID_LEN))
    {
        log_msg("사용자 명령: User ID가 비어 있다");
        return USERCMD_FAIL;
    }

    /*
     * 무엇이 붙어 왔는지는 **오브젝트가 아니라 길이로** 가린다.
     * 같은 오브젝트라도 DM이 이름·카드를 넣기도 하고 비우기도 하기 때문이다(`3.` 문서 2절 표).
     */
    const uint8_t *card32 = NULL;

    if (len >= IDTI_USERDATA_LEN)
    {
        /* UserData(112): Info + Name + Card + Restriction + Group */
        memcpy(r.lcd_name, data + IDTI_USERDATA_OFF_NAME, ACU_USER_NAME_LEN);
        card32 = data + IDTI_USERDATA_OFF_CARD;
        r.restrict_type        = data[IDTI_USERDATA_OFF_RESTRICT];
        r.restrict_limit_type  = data[IDTI_USERDATA_OFF_RESTRICT + 1];
        r.restrict_limit_count = data[IDTI_USERDATA_OFF_RESTRICT + 2];
        memcpy(r.group_codes, data + IDTI_USERDATA_OFF_GROUP, ACU_USER_GROUP_LEN);
    }
    else if (len >= IDTI_USERINFO_LEN + ACU_USER_NAME_LEN + IDTI_USERDATA_LEN_CARD)
    {
        /* Info + Name + Card (UserCard 0x17 계열) */
        memcpy(r.lcd_name, data + IDTI_USERDATA_OFF_NAME, ACU_USER_NAME_LEN);
        card32 = data + IDTI_USERDATA_OFF_CARD;
    }
    else if (len >= IDTI_USERINFO_LEN + ACU_USER_NAME_LEN)
    {
        /* Info + Name (UserInfo 0x16) */
        memcpy(r.lcd_name, data + IDTI_USERDATA_OFF_NAME, ACU_USER_NAME_LEN);
    }

    if (users_put(users, &r) != 0)
    {
        return USERCMD_FAIL;
    }

    if (card32 && !userbin_all_zero(card32, ACU_USER_CARD_LEN))
    {
        AcuUserCard c;
        memset(&c, 0, sizeof(c));
        memcpy(c.user_id, r.user_id, ACU_USER_ID_LEN);
        prox_to_card_id(card32, c.card_id);
        memcpy(c.prox_raw, card32, ACU_USER_PROX_LEN);

        if (users_put_card(users, &c) != 0)
        {
            return USERCMD_FAIL;
        }
    }

    char line[120];
    snprintf(line, sizeof(line), "사용자 명령: 1명 등록 (오브젝트 0x%02x, %zubyte)", object, len);
    log_msg(line);
    return USERCMD_OK;
}

AcuUserCmdStatus usercmd_delete(AcuUsers *users, const uint8_t *data, size_t len)
{
    if (!users || !data || len < ACU_USER_ID_LEN)
    {
        log_msg("사용자 명령: 삭제 데이터가 짧다");
        return USERCMD_FAIL;
    }

    /*
     * Revision ID는 받기만 하고 쓰지 않는다. 규약에 `00000001 고정`으로 적혀 있고
     * (`1.` 문서), 우리는 사용자 ID 하나로 명단을 다룬다.
     */
    if (users_delete(users, data) != 0)
    {
        return USERCMD_FAIL;
    }
    log_msg("사용자 명령: 1명 삭제");
    return USERCMD_OK;
}

AcuUserCmdStatus usercmd_get(AcuUsers *users, uint8_t object,
                            const uint8_t *data, size_t len,
                            uint8_t *out, size_t *out_len)
{
    if (out_len)
    {
        *out_len = 0;
    }
    if (!users || !data || !out || !out_len || len < ACU_USER_ID_LEN)
    {
        return USERCMD_FAIL;
    }

    AcuUserRecord r;
    if (users_lookup_by_id(users, data, &r) != 1)
    {
        log_msg("사용자 명령: 요청한 사용자가 없다");
        return USERCMD_FAIL;
    }

    /*
     * 요청한 오브젝트 그대로 만들어 돌려준다.
     * UserData(0x21)가 카드까지 담는 형태라 기본으로 쓰고, Info만 물으면 32byte만 보낸다.
     */
    memset(out, 0, IDTI_USERDATA_LEN);
    userbin_build_user_info(&r, out + IDTI_USERDATA_OFF_INFO);

    if (object == IDTI_OBJ_USER_INFO)
    {
        memcpy(out + IDTI_USERDATA_OFF_NAME, r.lcd_name, ACU_USER_NAME_LEN);
        *out_len = IDTI_USERINFO_LEN + ACU_USER_NAME_LEN;
        return USERCMD_OK;
    }

    memcpy(out + IDTI_USERDATA_OFF_NAME, r.lcd_name, ACU_USER_NAME_LEN);

    /* 카드는 사용자 ID로 되찾는다 (인증 수단 표에 따로 있다) */
    AcuUserCard card;
    if (users_lookup_card_by_user(users, r.user_id, &card) == 1)
    {
        card_id_to_prox(card.card_id, out + IDTI_USERDATA_OFF_CARD);
    }

    out[IDTI_USERDATA_OFF_RESTRICT]     = r.restrict_type;
    out[IDTI_USERDATA_OFF_RESTRICT + 1] = r.restrict_limit_type;
    out[IDTI_USERDATA_OFF_RESTRICT + 2] = r.restrict_limit_count;
    memcpy(out + IDTI_USERDATA_OFF_GROUP, r.group_codes, ACU_USER_GROUP_LEN);

    *out_len = IDTI_USERDATA_LEN;
    return USERCMD_OK;
}
