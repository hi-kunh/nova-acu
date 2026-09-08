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

/*
 * Frame Option 16bit (buf[4]<<8 | buf[5] 로 합친 값에서의 비트 위치).
 * 근거: PC 소스 `isldev/clsDevFrame.cs`의 BuildFrameOptionByte() + clsDevCommon.CalcBoolArrayToByte().
 * bool 배열 인덱스 0~7이 뒷바이트(buf[5]), 8~15가 앞바이트(buf[4])로 들어가고 바이트 안에서는 LSB부터 찬다.
 */
#define IDTI_FOPT_REQUEST_ACK           0x8000 /* buf[4] bit7 */
#define IDTI_FOPT_PASSWORD              0x4000 /* buf[4] bit6: 켜졌을 때만 Password 필드가 의미 있음 */
#define IDTI_FOPT_EXCLUDE_DEVICE_STATUS 0x0080 /* buf[5] bit7: 응답에서 Device Status를 빼라 */
#define IDTI_FOPT_TIME_SYNC             0x0040 /* buf[5] bit6: Event Request 시 시각 동기화 (미구현) */
#define IDTI_FOPT_RE_REQUEST_EVENT      0x0020 /* buf[5] bit5: 직전 이벤트 재요청 (미구현) */
#define IDTI_FOPT_CHECK_PACKET          0x0010 /* buf[5] bit4: Tail 4byte(CheckBytes 포함) 여부 */
#define IDTI_FOPT_TCP                   0x0001 /* buf[5] bit0 */

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

/* Object Table (일부) - 근거: PC 소스 `isldev/clsDevCommand.cs`의 DeviceObject enum */
#define IDTI_OBJ_HISTORY       0x01 /* Event log */
#define IDTI_OBJ_HISTORY_COUNT 0x05 /* 이벤트 개수 (아직 미구현) */
#define IDTI_OBJ_HISTORY_INDEX 0x06 /* 이벤트 읽기 위치 (아직 미구현) */
#define IDTI_OBJ_FIRMWARE      0x2A /* 42. PC가 접속 후 장치 상태를 물을 때 쓰는 오브젝트 */

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

/*
 * 장치 식별자. Device Status / Firmware Info 모두 앞 2byte가 [0]=Category, [1]=DeviceType 이다
 * (근거: PC 소스 `isldev/clsDevStatus.cs`, `clsDevDeviceSetting.cs`).
 * Category 값은 아직 확정 전 - 0이 유효한지 확인 필요. DeviceType은 hal.h의 HAL_DEVICE_TYPE_ISC101과 대응.
 */
#define IDTI_DEVICE_CATEGORY 0x00
#define IDTI_DEVICE_TYPE     0x29

/*
 * Firmware Info (Object 0x2A 응답 데이터):
 * Category(1) + DeviceType(1) + Version(4) + DateTime(6, BCD) + Reserved(256) = 268byte.
 * Version 4byte는 PC에서 각 byte를 10진수 2자리로 이어 붙여 표시한다 ({1,0,0,0} -> "01000000").
 */
#define IDTI_FIRMWARE_INFO_LEN 268
#define IDTI_FW_VERSION_MAJOR 1
#define IDTI_FW_VERSION_MINOR 0
#define IDTI_FW_VERSION_PATCH 0
#define IDTI_FW_VERSION_BUILD 0
/* 펌웨어 빌드 일시 (BCD로 나감). 릴리스할 때 갱신할 것 */
#define IDTI_FW_DATE_YEAR   26
#define IDTI_FW_DATE_MONTH   9
#define IDTI_FW_DATE_DAY     8
#define IDTI_FW_DATE_HOUR    0
#define IDTI_FW_DATE_MINUTE  0
#define IDTI_FW_DATE_SECOND  0

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
 * 응답 패킷(Header+Data+Tail)을 out에 만들어 넣는다. Tail은 항상 2byte로 만든다.
 * dest_addr/src_addr는 응답 패킷 기준 (우리가 보내는 쪽 Source, 상대가 Destination).
 * frame_option: 응답에 실을 Frame Option. IsRequestAck는 항상 세우고 IsCheckPacket은 항상 지운다
 *   (CheckBytes 4byte Tail을 만들지 않으므로). 요청의 IsExcludeDeviceStatus를 그대로 되돌려 주면
 *   PC가 응답 안에 Device Status가 들어 있는지 판단할 수 있다.
 * 반환: 패킷 전체 길이, out_cap이 부족하면 -1.
 */
int idti_build_packet(uint8_t *out, size_t out_cap,
                       const uint8_t dest_addr[IDTI_ADDR_DEST_LEN],
                       const uint8_t src_addr[IDTI_ADDR_SRC_LEN],
                       uint16_t frame_option,
                       uint32_t frame_index, uint32_t password,
                       uint8_t command, uint8_t sub_command, uint8_t object,
                       uint8_t start_item, uint8_t end_item,
                       uint16_t data_current, uint16_t data_end,
                       uint16_t data_total, uint16_t data_one_len,
                       const uint8_t *data, size_t data_len);

/* 8-bit 정수를 2자리 BCD(Binary Coded Decimal) 바이트로 변환한다 (0~99 범위) */
uint8_t idti_to_bcd(int value);

#endif /* ACU_PROTOCOL_H */
