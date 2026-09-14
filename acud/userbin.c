#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "userbin.h"
#include "protocol.h"
#include "log.h"

struct AcuUserBin {
    AcuUsers *users;

    int      active;          /* 받는 중 */
    uint32_t total_size;      /* Start가 알려 준 전체 byte 수 */
    uint32_t total_count;     /* Start가 알려 준 사용자 수 */
    uint32_t received;        /* 지금까지 받은 byte */
    size_t   record_len;      /* 128 또는 1808 */

    int      have_index;      /* 첫 조각을 받았는지 */
    uint16_t last_index;      /* 마지막으로 처리한 조각 번호 */

    /* 조각이 레코드 경계와 맞지 않으므로 걸치는 부분을 여기 모은다 */
    uint8_t  rec[IDTI_USERBIN_REC_INFO_FINGER_LEN];
    size_t   rec_len;

    uint32_t parsed_ok;       /* 저장에 성공한 사용자 수 */
    uint32_t parsed_bad;      /* 버린 사용자 수 (CRC 불일치 등) */
};

AcuUserBin *userbin_create(AcuUsers *users)
{
    if (!users)
    {
        return NULL;
    }
    AcuUserBin *ub = calloc(1, sizeof(AcuUserBin));
    if (ub)
    {
        ub->users = users;
    }
    return ub;
}

void userbin_destroy(AcuUserBin *ub)
{
    if (ub)
    {
        userbin_abort(ub);
        free(ub);
    }
}

int userbin_in_progress(const AcuUserBin *ub)
{
    return (ub && ub->active) ? 1 : 0;
}

void userbin_abort(AcuUserBin *ub)
{
    if (!ub || !ub->active)
    {
        return;
    }
    users_load_abort(ub->users);
    ub->active = 0;
    ub->rec_len = 0;
    ub->received = 0;
    ub->have_index = 0;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

int userbin_start(AcuUserBin *ub, uint32_t total_size, uint32_t total_count, uint8_t binary_obj)
{
    if (!ub)
    {
        return -1;
    }

    if (ub->active)
    {
        log_msg("사용자 바이너리: 받던 것이 있어 버리고 새로 시작한다");
        userbin_abort(ub);
    }

    size_t rec_len;
    switch (binary_obj)
    {
        case IDTI_USERBIN_OBJ_INFO:
            rec_len = IDTI_USERBIN_REC_INFO_LEN;
            break;
        case IDTI_USERBIN_OBJ_INFO_FINGER:
            /*
             * 지문 포함 구조(1808byte). 우리는 카드 전용이라 지문 1,680byte는 읽고 버린다 —
             * 지금 현장 IDTi는 지문을 쓰지 않는다. 보관 여부는 생체 장치 선정 후에 정한다
             * (HARDWARE.md "확장 고려").
             */
            rec_len = IDTI_USERBIN_REC_INFO_FINGER_LEN;
            break;
        default:
        {
            char line[160];
            snprintf(line, sizeof(line),
                     "사용자 바이너리: 모르는 Binary OBJ 0x%02x - 거절", binary_obj);
            log_msg(line);
            return -1;
        }
    }

    if (total_size == 0 || total_size % rec_len != 0)
    {
        /*
         * 총 크기가 레코드 크기로 나누어떨어지지 않으면 우리가 아는 구조가 아니다.
         * 반쪽 명단으로 교체하는 것보다 거절하는 편이 낫다.
         */
        char line[200];
        snprintf(line, sizeof(line),
                 "사용자 바이너리: 총 크기 %u가 레코드 %zu의 배수가 아니다 - 거절",
                 total_size, rec_len);
        log_msg(line);
        return -1;
    }

    if (users_load_begin(ub->users) != 0)
    {
        log_msg("사용자 바이너리: 받을 표를 만들지 못했다");
        return -1;
    }

    ub->active = 1;
    ub->total_size = total_size;
    ub->total_count = total_count;
    ub->record_len = rec_len;
    ub->received = 0;
    ub->rec_len = 0;
    ub->have_index = 0;
    ub->last_index = 0;
    ub->parsed_ok = 0;
    ub->parsed_bad = 0;

    char line[220];
    snprintf(line, sizeof(line),
             "사용자 바이너리: 전송 시작 - %u명 / %u byte / 레코드 %zubyte (OBJ 0x%02x)",
             total_count, total_size, rec_len, binary_obj);
    log_msg(line);
    return 0;
}

/*
 * 레코드의 검사값을 확인한다.
 * `datacrc` = user[0]부터 crc_calc까지의 XOR (`5.` 문서 구조체 주석).
 * 지문 구조(1808)도 앞 128byte 배치는 같으므로 같은 방식으로 본다.
 */
static int record_crc_ok(const uint8_t *rec)
{
    uint8_t x = 0;
    for (int i = IDTI_USERBIN_OFF_USER; i <= IDTI_USERBIN_OFF_CRC_CALC; i++)
    {
        x ^= rec[i];
    }
    return (x == rec[IDTI_USERBIN_OFF_DATACRC]) ? 1 : 0;
}

/* User Info 32byte를 레코드 구조체로 옮긴다 (`3.` 문서 A절) */
void userbin_parse_user_info(const uint8_t *info, AcuUserRecord *r)
{
    memcpy(r->user_id, info + IDTI_USERINFO_OFF_ID, ACU_USER_ID_LEN);
    r->general_group   = info[IDTI_USERINFO_OFF_GENGROUP];
    r->revision_id     = be16(info + IDTI_USERINFO_OFF_REVISION);
    r->access_option   = be32(info + IDTI_USERINFO_OFF_OPTION);
    r->level           = info[IDTI_USERINFO_OFF_LEVEL];
    r->validation_code = be16(info + IDTI_USERINFO_OFF_VALIDATION);
    r->timezone_code   = be16(info + IDTI_USERINFO_OFF_TIMEZONE);
    r->expired_date    = ((uint32_t)info[IDTI_USERINFO_OFF_EXPIRED] << 16) |
                         ((uint32_t)info[IDTI_USERINFO_OFF_EXPIRED + 1] << 8) |
                         info[IDTI_USERINFO_OFF_EXPIRED + 2];
    r->prox_type       = info[IDTI_USERINFO_OFF_PROXTYPE];
    r->prox_wiegand    = info[IDTI_USERINFO_OFF_PROXWIEG];
    r->bio_type        = info[IDTI_USERINFO_OFF_BIOTYPE];
    r->bio_type_sub    = info[IDTI_USERINFO_OFF_BIOSUB];
    r->template_count  = info[IDTI_USERINFO_OFF_TMPLCOUNT];
    r->access_password = be16(info + IDTI_USERINFO_OFF_PASSWORD);
    r->canteen_code    = info[IDTI_USERINFO_OFF_CANTEEN];
}

/* 레코드 구조체를 User Info 32byte로 되돌린다 (되돌려 줄 때 쓴다) */
void userbin_build_user_info(const AcuUserRecord *r, uint8_t *info)
{
    memset(info, 0, IDTI_USERINFO_LEN);
    memcpy(info + IDTI_USERINFO_OFF_ID, r->user_id, ACU_USER_ID_LEN);
    info[IDTI_USERINFO_OFF_GENGROUP] = r->general_group;
    info[IDTI_USERINFO_OFF_REVISION]     = (uint8_t)(r->revision_id >> 8);
    info[IDTI_USERINFO_OFF_REVISION + 1] = (uint8_t)r->revision_id;
    info[IDTI_USERINFO_OFF_OPTION]     = (uint8_t)(r->access_option >> 24);
    info[IDTI_USERINFO_OFF_OPTION + 1] = (uint8_t)(r->access_option >> 16);
    info[IDTI_USERINFO_OFF_OPTION + 2] = (uint8_t)(r->access_option >> 8);
    info[IDTI_USERINFO_OFF_OPTION + 3] = (uint8_t)r->access_option;
    info[IDTI_USERINFO_OFF_LEVEL] = r->level;
    info[IDTI_USERINFO_OFF_VALIDATION]     = (uint8_t)(r->validation_code >> 8);
    info[IDTI_USERINFO_OFF_VALIDATION + 1] = (uint8_t)r->validation_code;
    info[IDTI_USERINFO_OFF_TIMEZONE]     = (uint8_t)(r->timezone_code >> 8);
    info[IDTI_USERINFO_OFF_TIMEZONE + 1] = (uint8_t)r->timezone_code;
    info[IDTI_USERINFO_OFF_EXPIRED]     = (uint8_t)(r->expired_date >> 16);
    info[IDTI_USERINFO_OFF_EXPIRED + 1] = (uint8_t)(r->expired_date >> 8);
    info[IDTI_USERINFO_OFF_EXPIRED + 2] = (uint8_t)r->expired_date;
    info[IDTI_USERINFO_OFF_PROXTYPE]  = r->prox_type;
    info[IDTI_USERINFO_OFF_PROXWIEG]  = r->prox_wiegand;
    info[IDTI_USERINFO_OFF_BIOTYPE]   = r->bio_type;
    info[IDTI_USERINFO_OFF_BIOSUB]    = r->bio_type_sub;
    info[IDTI_USERINFO_OFF_TMPLCOUNT] = r->template_count;
    info[IDTI_USERINFO_OFF_PASSWORD]     = (uint8_t)(r->access_password >> 8);
    info[IDTI_USERINFO_OFF_PASSWORD + 1] = (uint8_t)r->access_password;
    info[IDTI_USERINFO_OFF_CANTEEN] = r->canteen_code;
}

/* 카드값이 비어 있는지 (등록된 카드가 없는 사용자) */
int userbin_all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        if (p[i] != 0)
        {
            return 0;
        }
    }
    return 1;
}

/* 레코드 한 개를 저장소에 넣는다. 반환: 0=넣음, -1=버림 */
static int store_record(AcuUserBin *ub, const uint8_t *rec)
{
    if (!record_crc_ok(rec))
    {
        return -1;
    }

    AcuUserRecord r;
    memset(&r, 0, sizeof(r));
    userbin_parse_user_info(rec + IDTI_USERBIN_OFF_USER, &r);

    if (userbin_all_zero(r.user_id, ACU_USER_ID_LEN))
    {
        return -1; /* 빈 칸 — 명단의 사용하지 않는 자리다 */
    }

    memcpy(r.lcd_name, rec + IDTI_USERBIN_OFF_NAME, ACU_USER_NAME_LEN);
    memcpy(r.group_codes, rec + IDTI_USERBIN_OFF_GROUP, ACU_USER_GROUP_LEN);

    /* Restriction은 바이너리에서 8byte다 (단일 사용자 경로는 16byte). 앞 3개만 의미가 있다 */
    r.restrict_type        = rec[IDTI_USERBIN_OFF_RESTRICT];
    r.restrict_limit_type  = rec[IDTI_USERBIN_OFF_RESTRICT + 1];
    r.restrict_limit_count = rec[IDTI_USERBIN_OFF_RESTRICT + 2];

    if (users_load_put(ub->users, &r) != 0)
    {
        return -1;
    }

    /*
     * 카드.
     *
     * 두 경로가 **바이트 순서가 서로 뒤집힌 채로** 같은 값을 나른다:
     *   바이너리 전송     card[0..7]        = 카드 번호를 **큰 자리부터**(빅엔디언, 오른쪽 정렬)
     *   단일 사용자 경로  Proximity Data    = 그것을 뒤집은 형태
     *
     * 근거 둘:
     *   · `clsDevUserBin.cs` — `Copy(8) -> Reverse -> Prox32[0..7]` (둘이 서로 뒤집힘)
     *   · `1.` 문서 — Card ID 예시에 **"(Reversed Bytes)"** 라고 적혀 있다:
     *     `0xc1/0x5d/0x06/0x02` 로 실린 것이 실제로는 `02/06/5d/c1` 이다
     * ⇒ 뒤집힌 쪽은 Proximity Data이고, **바이너리의 card[0..7]이 바로 카드 번호**다.
     *
     * 그래서 판정에 쓰는 `card_id`는 바이너리 값을 **그대로** 쓴다. 이벤트의 Access ID도
     * 같은 큰 자리부터 형식이라(`1.` 문서 예시 `00/00/00/00/00/00/12/34`) 표현이 하나로 맞는다.
     * `prox_raw`에는 단일 사용자 경로가 쓰는 뒤집힌 형태를 넣어 둔다 — 되돌려 줄 때 필요하다.
     */
    const uint8_t *bin_card = rec + IDTI_USERBIN_OFF_CARD;
    if (!userbin_all_zero(bin_card, ACU_USER_CARD_LEN))
    {
        AcuUserCard c;
        memset(&c, 0, sizeof(c));
        memcpy(c.user_id, r.user_id, ACU_USER_ID_LEN);
        memcpy(c.card_id, bin_card, ACU_USER_CARD_LEN);
        for (int i = 0; i < ACU_USER_CARD_LEN; i++)
        {
            c.prox_raw[i] = bin_card[ACU_USER_CARD_LEN - 1 - i];
        }

        if (users_load_put_card(ub->users, &c) != 0)
        {
            return -1;
        }
    }
    return 0;
}

/* 다 받았다 - 명단을 교체하고 결과를 정리한다 */
static AcuUserBinStatus finish(AcuUserBin *ub)
{
    int rc = users_load_commit(ub->users);
    ub->active = 0;
    ub->rec_len = 0;

    char line[240];
    if (rc != 0)
    {
        ub->parsed_ok = 0;
        snprintf(line, sizeof(line), "사용자 바이너리: 명단 교체 실패 - 기존 명단을 유지한다");
        log_msg(line);
        return USERBIN_COMPLETE; /* 전송 자체는 끝났다. 결과 이벤트는 '전부 실패'가 된다 */
    }

    snprintf(line, sizeof(line),
             "사용자 바이너리: 완료 - %u명 등록 (알린 인원 %u, 버린 레코드 %u)",
             ub->parsed_ok, ub->total_count, ub->parsed_bad);
    log_msg(line);
    return USERBIN_COMPLETE;
}

AcuUserBinStatus userbin_continue(AcuUserBin *ub, uint16_t index,
                                  const uint8_t *data, size_t len)
{
    if (!ub || !ub->active || !data)
    {
        log_msg("사용자 바이너리: Start 없이 조각이 왔다 - 거절");
        return USERBIN_FAILED;
    }

    /*
     * 조각 번호. PC는 Fail을 받으면 **직전 조각을 다시 보낸다.**
     * 우리가 Fail로 답한 조각은 처리하지 않았으므로 다시 오면 정상 경로로 들어온다.
     * 반대로 우리 응답이 유실돼 **이미 처리한 조각이 다시 오면** 그냥 성공으로 답한다
     * (두 번 넣으면 같은 사용자가 겹쳐 들어간다).
     */
    if (ub->have_index)
    {
        if (index == ub->last_index)
        {
            log_msg("사용자 바이너리: 같은 조각이 다시 왔다 - 이미 처리한 것이라 넘긴다");
            return USERBIN_OK;
        }
        if (index != (uint16_t)(ub->last_index + 1))
        {
            char line[200];
            snprintf(line, sizeof(line),
                     "사용자 바이너리: 조각 번호가 건너뛰었다 (기대 %u, 받음 %u) - Fail로 답한다",
                     (unsigned)(uint16_t)(ub->last_index + 1), (unsigned)index);
            log_msg(line);
            return USERBIN_FAILED;
        }
    }

    if (ub->received + len > ub->total_size)
    {
        /* 알린 크기보다 많이 왔다. 우리가 아는 구조가 아니므로 자르지 않고 거절한다 */
        log_msg("사용자 바이너리: 알린 총 크기보다 많이 왔다 - Fail로 답한다");
        return USERBIN_FAILED;
    }

    /* 바이트 흐름을 레코드 크기로 잘라 한 명씩 해석한다 */
    size_t off = 0;
    while (off < len)
    {
        size_t need = ub->record_len - ub->rec_len;
        size_t take = (len - off < need) ? (len - off) : need;

        memcpy(ub->rec + ub->rec_len, data + off, take);
        ub->rec_len += take;
        off += take;

        if (ub->rec_len == ub->record_len)
        {
            if (store_record(ub, ub->rec) == 0)
            {
                ub->parsed_ok++;
            }
            else
            {
                ub->parsed_bad++;
            }
            ub->rec_len = 0;
        }
    }

    ub->received += (uint32_t)len;
    ub->last_index = index;
    ub->have_index = 1;

    if (ub->received >= ub->total_size)
    {
        return finish(ub);
    }
    return USERBIN_OK;
}

uint32_t userbin_result_event(const AcuUserBin *ub)
{
    if (!ub || ub->parsed_ok == 0)
    {
        return IDTI_EVENT_USERFILE_FAIL;
    }
    if (ub->parsed_bad > 0)
    {
        return IDTI_EVENT_USERFILE_PARTIAL;
    }
    return IDTI_EVENT_USERFILE_SUCCESS;
}
