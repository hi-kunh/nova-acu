#ifndef ACU_USERS_H
#define ACU_USERS_H

#include <stdint.h>

/*
 * 사용자 저장 (`users.db`).
 *
 * **왜 따로 두는가** — 1단계에 만든 `cards` 표는 판정 로직을 확인하려고 급히 만든 것이라
 * 규약 필드와 모양이 다르다. DM이 내려보내는 것은 `3. IDTi Protocol Member(User DB) Structure`의
 * 레코드이고, 그대로 받아 두지 않으면 되돌려 줄 때(`UserReceive`) 값이 달라진다.
 *
 * 파일도 이벤트·설정과 나눈다. 사용자 DB는 **수십만 건을 통째로 교체**하는 쓰기, 이벤트는
 * 한 건씩 붙는 쓰기다. 전체 다운로드가 이벤트 적재를 붙잡으면 안 되고,
 * 나중에 OS를 A/B로 가를 때 사용자·이벤트는 **A/B 밖 데이터 파티션**에 남아야 한다.
 *
 * **규모** — DM은 10만 명(지문 모델 2만)을 넘으면 전송을 시작하지 않는다(DM 회신 7절).
 * 장비 사양은 26만(SSC-334)이다. 여기 설계는 100만 명까지 버티는 것을 목표로 한다.
 *
 * **사용자 ID 중심** — 카드는 `user_cards`라는 **인증 수단 표**에 따로 둔다. 지금은 카드뿐이지만
 * 나중에 지문·안면을 같은 모양의 표로 붙일 수 있다 (HARDWARE.md "확장 고려").
 */

typedef struct AcuUsers AcuUsers;

/*
 * AccessOption의 **Enable 비트** — 이 비트가 꺼져 있으면 등록돼 있어도 출입을 거부한다.
 *
 * 값의 근거: SDK(`clsDevUser.cs`)는 `boolArray[31] = AccessOptionIsEnable`로 쓰고,
 * `clsDevCommon.CalcBoolArrayToByte`가 8개씩 묶어 **바이트 순서를 뒤집어**(`retVal.Length - i - 1`)
 * 담는다. 그래서 인덱스 31은 4byte 중 **첫 바이트의 최상위 비트**가 되고,
 * 4byte를 빅엔디언 정수로 읽으면 `0x80000000`이다.
 *
 * ⚠ **SDK 소스에서 유도한 값이다.** 사용자 레코드 원시 바이트를 DM에서 받아 확인해야 한다
 * (dm/README.md 실측 요청 목록). 지금은 mock 데이터라 양쪽이 같은 정의를 쓰므로 문제가 없지만,
 * 실제 DM 다운로드를 붙일 때 가장 먼저 확인할 값이다.
 */
#define ACU_USER_OPT_ENABLE 0x80000000u

#define ACU_USER_ID_LEN     8   /* 규약 User ID */
#define ACU_USER_NAME_LEN  16   /* User LCD Initials Data */
#define ACU_USER_GROUP_LEN 16   /* User Group Data = 2byte x 8 */
#define ACU_USER_PROX_LEN  32   /* User Proximity Data (원본) */
#define ACU_USER_CARD_LEN   8   /* 실제 카드값 (islprox lengthRealProxByte) */

/*
 * 사용자 한 명. 필드 이름과 크기는 규약 그대로다
 * (`isldev\clsDevUser.cs`의 lengthUserInfo*Byte — 합이 User Info 32byte).
 */
typedef struct {
    uint8_t  user_id[ACU_USER_ID_LEN];
    uint8_t  general_group;     /* UserGeneralGroupCode (1) */
    uint16_t revision_id;       /* UserRevisionID (2) */
    uint32_t access_option;     /* AccessOption (4, 비트) - Enable 비트가 판정에 쓰인다 */
    uint8_t  level;             /* Level (1) */
    uint16_t validation_code;   /* ValidationCode (2) - 0이면 제한 없음 */
    uint16_t timezone_code;     /* TimezoneCode (2) - 0이면 제한 없음 */
    uint32_t expired_date;      /* AccessExpiredDate (3) - 0xffffff면 없음 */
    uint8_t  prox_type;         /* ProximityType (1) */
    uint8_t  prox_wiegand;      /* ProximityWiegand (1) */
    uint8_t  bio_type;          /* BiometricType (1) */
    uint8_t  bio_type_sub;      /* BiometricTypeSub (1) */
    uint8_t  template_count;    /* TemplateCount (1) - 지문 개수 */
    uint16_t access_password;   /* AccessPassword (2) */
    uint8_t  canteen_code;      /* CanteenCode (1) */

    uint8_t  lcd_name[ACU_USER_NAME_LEN];   /* NCU에는 LCD가 없지만 DM이 보내므로 그대로 보관한다 */

    uint8_t  restrict_type;        /* Restriction[0] */
    uint8_t  restrict_limit_type;  /* Restriction[1] */
    uint8_t  restrict_limit_count; /* Restriction[2] */

    uint8_t  group_codes[ACU_USER_GROUP_LEN]; /* 2byte x 8 */
} AcuUserRecord;

/* 인증 수단 — 카드 하나 */
typedef struct {
    uint8_t card_id[ACU_USER_CARD_LEN];  /* 실제 카드값. 판정은 이 값으로 찾는다 */
    uint8_t user_id[ACU_USER_ID_LEN];
    uint8_t prox_raw[ACU_USER_PROX_LEN]; /* 규약 Proximity Data 원본 (형식이 prox_type/wiegand에 달렸다) */
} AcuUserCard;

/* 연다 (스키마가 없으면 만든다). 실패 시 NULL — 사용자 DB 없이는 판정이 불가능하다 */
AcuUsers *users_open(const char *path);

/* 정리한다. u가 NULL이어도 안전 */
void users_close(AcuUsers *u);

/*
 * 카드값으로 사용자를 찾는다 (판정 경로 — 가장 자주 불린다).
 * 반환: 1=찾음, 0=없음, -1=오류
 */
int users_lookup_by_card(AcuUsers *u, const uint8_t card_id[ACU_USER_CARD_LEN],
                         AcuUserRecord *out);

/* 사용자 ID로 찾는다 (개별 조회·삭제·되돌려주기용). 반환: 1=찾음, 0=없음, -1=오류 */
int users_lookup_by_id(AcuUsers *u, const uint8_t user_id[ACU_USER_ID_LEN],
                       AcuUserRecord *out);

/* 등록된 사용자 수. 오류면 -1 */
long long users_count(const AcuUsers *u);

/* ---- 개별 변경 (DM의 UserSend/UserDelete) — 즉시 반영한다 ---- */

int users_put(AcuUsers *u, const AcuUserRecord *rec);
int users_put_card(AcuUsers *u, const AcuUserCard *card);
int users_delete(AcuUsers *u, const uint8_t user_id[ACU_USER_ID_LEN]);

/*
 * ---- 전체 다운로드 (DM의 UserBinTransSendStart/Continue) ----
 *
 * **새 표에 받아 끝나면 한 번에 바꿔 끼운다.** 받는 동안에도 기존 사용자로 출입 판정이 계속되고,
 * 중간에 끊기거나 정전이 나도 반쪽짜리 명단이 남지 않는다. 교체는 한 트랜잭션이다.
 *
 * 1명씩 커밋하면 26만 명에 한 시간이 걸린다(보드 실측 13.6ms/명) — 그래서 안에서 묶어 커밋한다.
 */
int users_load_begin(AcuUsers *u);
int users_load_put(AcuUsers *u, const AcuUserRecord *rec);
int users_load_put_card(AcuUsers *u, const AcuUserCard *card);
int users_load_commit(AcuUsers *u); /* 여기서 교체된다 */
void users_load_abort(AcuUsers *u); /* 받다 만 것을 버린다 (연결이 끊겼을 때) */

/* 지금 전체 다운로드를 받는 중인지 (1=받는 중) */
int users_load_in_progress(const AcuUsers *u);

/*
 * 테스트 단계용: mock 카드 5장을 없을 때만 채워 넣는다 (기존 `db_seed_dummy_data`의 사용자 쪽).
 * 유효기간·시간대 그룹은 설정이라 `acud.db`에 그대로 있다.
 */
void users_seed_dummy(AcuUsers *u);

#endif /* ACU_USERS_H */
