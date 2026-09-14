/*
 * ACU↔RRU 프로토콜 문서의 **프레임 규칙과 참조 코드가 실제로 맞는지** 검산한다.
 * 대상 문서: rru/ACU_RRU_USB_프로토콜_설계_초안.md (9-4 예시 · 9-5 참조 코드 · 부록 A CRC)
 *
 * 문서에 적은 코드를 **그대로 복사해** 넣었다 — 문서를 고치면 여기도 같이 고쳐서
 * "문서에 적힌 대로 짜면 정말 되는가"를 계속 확인할 수 있게 한다.
 *
 * 빌드·실행: make -C acud rrucheck && ./acud/rru_proto_check
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

uint16_t rru_crc16(const uint8_t *p, uint16_t n)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < n; i++) {
        crc ^= (uint16_t)p[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

#define RRU_MAX_PAYLOAD 255
#define RRU_FRAME_MAX   (1 + 2 + 2 + RRU_MAX_PAYLOAD + 2 + 1)

typedef struct { uint8_t buf[RRU_FRAME_MAX]; uint16_t len; uint16_t want; } RruRx;

static int rru_rx_byte(RruRx *r, uint8_t b)
{
    if (r->len == 0) {
        if (b != 0x02) return 0;
        r->buf[r->len++] = b; r->want = 0; return 0;
    }
    r->buf[r->len++] = b;
    if (r->len == 3) {
        uint16_t body = (uint16_t)(r->buf[1] << 8 | r->buf[2]);
        if (body < 2 || body > 2 + RRU_MAX_PAYLOAD) { r->len = 0; return 0; }
        r->want = (uint16_t)(3 + body + 3);
    }
    if (r->want == 0 || r->len < r->want) return 0;
    uint16_t body = (uint16_t)(r->want - 6);
    uint16_t got  = (uint16_t)(r->buf[3 + body] << 8 | r->buf[3 + body + 1]);
    int ok = (r->buf[r->want - 1] == 0x03) && (got == rru_crc16(&r->buf[3], body));
    r->len = 0;
    return ok;
}

static uint16_t rru_build(uint8_t *out, uint8_t seq, uint8_t type,
                          const uint8_t *payload, uint8_t n)
{
    uint16_t body = (uint16_t)(2 + n);
    out[0] = 0x02;
    out[1] = (uint8_t)(body >> 8);
    out[2] = (uint8_t)body;
    out[3] = seq; out[4] = type;
    if (n) memcpy(out + 5, payload, n);
    uint16_t crc = rru_crc16(out + 3, body);
    out[3 + body] = (uint8_t)(crc >> 8);
    out[3 + body + 1] = (uint8_t)crc;
    out[3 + body + 2] = 0x03;
    return (uint16_t)(3 + body + 3);
}

static int fails = 0;
static void want(const char *what, int got, int exp)
{
    if (got == exp) printf("  ✓ %s\n", what);
    else { printf("  ✗ %s : %d (기대 %d)\n", what, got, exp); fails++; }
}

/* 만든 프레임을 파서에 한 byte씩 넣어 본다 */
static int roundtrip(uint8_t seq, uint8_t type, const uint8_t *pl, uint8_t n, uint16_t *out_total)
{
    uint8_t f[RRU_FRAME_MAX]; RruRx rx = {0};
    uint16_t total = rru_build(f, seq, type, pl, n);
    if (out_total) *out_total = total;
    int done = 0;
    for (uint16_t i = 0; i < total; i++) done = rru_rx_byte(&rx, f[i]);
    return done;
}

int main(void)
{
    printf("CRC 검사값\n");
    want("'123456789' -> 0x29B1", rru_crc16((const uint8_t*)"123456789", 9), 0x29B1);

    printf("\n문서 9-4의 프레임 길이\n");
    uint16_t t;
    uint8_t ev[20] = {0,0,0,0x2A, 0x01,0x02,0x1A, 0x00,0x01,0x86,0xA0, 0x08,
                      0,0,0,0, 0x02,0x06,0x5D,0xC1};
    roundtrip(7, 0x10, ev, sizeof(ev), &t);
    want("EVENT  payload 20 -> 전체 28byte (LEN=22)", t, 28);

    uint8_t out4[4] = {0x01, 0x02, 0x00, 0x1E};
    roundtrip(12, 0x20, out4, 4, &t);
    want("OUTPUT payload  4 -> 전체 12byte (LEN=6)", t, 12);

    roundtrip(0x20, 0x30, NULL, 0, &t);
    want("PING   payload  0 -> 전체  8byte (LEN=2)", t, 8);

    uint8_t ident[24] = {0};
    roundtrip(1, 0x02, ident, 24, &t);
    want("IDENT  payload 24 -> 전체 32byte (LEN=26)", t, 32);

    uint8_t st[8] = {0};
    roundtrip(33, 0x41, st, 8, &t);
    want("STATE  payload  8 -> 전체 16byte (LEN=10)", t, 16);

    printf("\n파서\n");
    want("정상 프레임을 받는다", roundtrip(7, 0x10, ev, sizeof(ev), NULL), 1);

    /* payload에 STX/ETX가 섞여도 되는지 */
    uint8_t tricky[8] = {0x02, 0x03, 0x02, 0x02, 0x03, 0x03, 0x02, 0x03};
    want("payload에 STX·ETX가 있어도 받는다", roundtrip(9, 0x10, tricky, 8, NULL), 1);

    /* CRC를 망가뜨리면 버리는지 */
    {
        uint8_t f[64]; RruRx rx = {0};
        uint16_t total = rru_build(f, 5, 0x20, out4, 4);
        f[total - 3] ^= 0xFF;            /* CRC 훼손 */
        int done = 0;
        for (uint16_t i = 0; i < total; i++) done = rru_rx_byte(&rx, f[i]);
        want("CRC가 틀리면 버린다", done, 0);
    }

    /* 쓰레기 byte가 앞에 붙어도 다음 프레임을 찾는지 */
    {
        uint8_t f[64]; RruRx rx = {0};
        uint16_t total = rru_build(f, 5, 0x20, out4, 4);
        uint8_t junk[5] = {0xFF, 0x00, 0x03, 0xAB, 0x11};
        int done = 0;
        for (int i = 0; i < 5; i++) rru_rx_byte(&rx, junk[i]);
        for (uint16_t i = 0; i < total; i++) done = rru_rx_byte(&rx, f[i]);
        want("앞에 쓰레기가 있어도 다음 프레임을 찾는다", done, 1);
    }

    /* 깨진 프레임 뒤에 정상 프레임이 와도 회복하는지 */
    {
        uint8_t f[64], g[64]; RruRx rx = {0};
        uint16_t tf = rru_build(f, 5, 0x20, out4, 4);
        uint16_t tg = rru_build(g, 6, 0x30, NULL, 0);
        f[tf - 3] ^= 0xFF;
        int done = 0;
        for (uint16_t i = 0; i < tf; i++) rru_rx_byte(&rx, f[i]);
        for (uint16_t i = 0; i < tg; i++) done = rru_rx_byte(&rx, g[i]);
        want("깨진 프레임 뒤 정상 프레임을 받는다", done, 1);
    }

    printf("\n%s\n", fails ? "실패 있음" : "전부 통과");
    return fails ? 1 : 0;
}
