#ifndef ACU_PROTOCOL_H
#define ACU_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

/*
 * IDTi 프로토콜 V2 패킷 프레임 상수/구조체 (5단계: 네트워크 통신부).
 * 참고 문서: "1. IDTi Protocol V1_2_3 Basic structure.doc", "2. IDTi Protocol Event Structure & Event Code.doc"
 *
 * V2/V3는 헤더 구조(44byte, Address 13byte)가 동일하고 Device Status 크기만 다르다(V2=234byte, V3=1100byte,
 * 최대 28/128 리더 지원). 우리 장치는 단일 도어/단일 리더 컨트롤러이므로 V2를 사용한다(IO 확장 모듈 없음
 * -> ExistedModule=0, IOModuleStatus 전부 0으로 채움).
 */

#define IDTI_STX 0x02
#define IDTI_ETX 0x03
#define IDTI_PACKET_CHECKSUM_FIXED 0x08 /* Tail의 Packet Checksum은 고정값(계산 안 함) */

#define IDTI_PROTOCOL_VERSION_V2 2

#define IDTI_HEADER_LEN_V2 44 /* Header Length 필드에 들어가는 값 (V2/V3: 44byte) */

#define IDTI_ADDR_DEST_LEN 8 /* V2/V3 Destination Address: Host/ComSlot/Controller/Module/Device(4, bit연산) */
#define IDTI_ADDR_SRC_LEN  5 /* Source Address: Host/ComSlot/Controller/Module/Device */

/* Command Table (일부, 우리가 실제로 쓰는 것만) */
#define IDTI_CMD_SND_STATUS 0x03
#define IDTI_CMD_REQ_STATUS 0x04
#define IDTI_CMD_SND_DATA   0x05
#define IDTI_CMD_REQ_DATA   0x06

/* Sub Command Table (일부) */
#define IDTI_SUBCMD_READ   0x02
#define IDTI_SUBCMD_WRITE  0x03
#define IDTI_SUBCMD_DELETE 0x04
#define IDTI_SUBCMD_CHANGE 0x05
#define IDTI_SUBCMD_INIT   0x06

/* Object Table (일부) */
#define IDTI_OBJ_HISTORY 0x01 /* Event log */

/* Event Code (4byte, big-endian) - "2. Event Structure & Event Code.doc" 참고 */
#define IDTI_EVENT_ACCESS_AUTH_BY_CARD       0x01010102u
#define IDTI_EVENT_ACCESS_DENIED_BY_CARD     0x01020102u
#define IDTI_EVENT_ACCESS_DENIED_NOT_ENABLED 0x01020108u
#define IDTI_EVENT_ACCESS_DENIED_BY_TIME     0x01020109u

/* Operation Mode (Event Info 내) */
#define IDTI_OPMODE_CARD 0x02

/* Door Status */
#define IDTI_DOOR_STATUS_NONE   0x00
#define IDTI_DOOR_STATUS_OPEN   0x01 /* Not Closed */
#define IDTI_DOOR_STATUS_CLOSED 0x02

/* Function Code */
#define IDTI_FUNC_NONE 0xFF

/* Device Type (Device Status 구조체 내, hal.h의 HAL_DEVICE_TYPE_ISC101과 대응) */
#define IDTI_DEVICE_TYPE_ISC101 0x0029

#define IDTI_EVENT_INFO_LEN 36        /* Event Info(Data) 크기 */
#define IDTI_DEVICE_STATUS_V2_LEN 234 /* Protocol V2 Device Status 크기 (10 + 16*14) */

/* 파싱된 요청 헤더 (44byte 중 우리가 실제로 쓰는 필드만 native 타입으로 보관) */
typedef struct {
    uint16_t packet_length;
    uint8_t  protocol_version;
    uint16_t frame_option;
    uint8_t  dest_addr[IDTI_ADDR_DEST_LEN];
    uint8_t  src_addr[IDTI_ADDR_SRC_LEN];
    uint32_t frame_index;
    uint32_t password;
    uint8_t  command;
    uint8_t  sub_command;
    uint8_t  object;
    uint8_t  start_item;
    uint8_t  end_item;
    uint16_t data_block_current;
    uint16_t data_block_end;
    uint16_t data_block_total;
    uint16_t data_block_one_len;
    size_t   data_len; /* Packet Length로부터 계산한 실제 Data 부분 길이 */
} IdtiHeader;

/*
 * buf에서 헤더(44byte)를 파싱한다. STX/Header Length/Header Checksum을 검증한다.
 * 반환: 0=성공, -1=형식 오류. 성공 시 out->data_len에 (packet_length - 44 - tail_len)을 채운다.
 * tail_len은 IsCheckPacket 비트(Frame Option 2번째 byte bit4)에 따라 2 또는 4.
 */
int idti_header_parse(const uint8_t *buf, size_t len, IdtiHeader *out);

/*
 * 응답 패킷(Header+Data+Tail)을 out에 만들어 넣는다. Tail은 항상 2byte(IsCheckPacket=0)로 만든다.
 * dest_addr/src_addr는 응답 패킷 기준 (우리가 보내는 쪽 Source, 상대가 Destination).
 * 반환: 패킷 전체 길이, out_cap이 부족하면 -1.
 */
int idti_build_packet(uint8_t *out, size_t out_cap,
                       const uint8_t dest_addr[IDTI_ADDR_DEST_LEN],
                       const uint8_t src_addr[IDTI_ADDR_SRC_LEN],
                       uint32_t frame_index, uint32_t password,
                       uint8_t command, uint8_t sub_command, uint8_t object,
                       uint8_t start_item, uint8_t end_item,
                       uint16_t data_current, uint16_t data_end,
                       uint16_t data_total, uint16_t data_one_len,
                       const uint8_t *data, size_t data_len);

/* 8-bit 정수를 2자리 BCD(Binary Coded Decimal) 바이트로 변환한다 (0~99 범위) */
uint8_t idti_to_bcd(int value);

#endif /* ACU_PROTOCOL_H */
