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
#define IDTI_OBJ_LCD_CONTROL   0x31 /* 49. LCD 백라이트/날짜 형식. 우리 장비에는 LCD가 없다 */
#define IDTI_OBJ_MULTI_LANGUAGE 0xA5 /* 165. LCD 표시 언어. 우리 장비에는 LCD가 없다 */

/*
 * 설정 명령에 대한 ACK 결과 (clsDevParams.AckResult). 응답 Data에 1byte로 싣는다.
 * (근거: IntelliScan Interphone SDK의 장치 측 ACK 조립 코드)
 */
#define IDTI_ACK_SUCCESS 0x01
#define IDTI_ACK_FAIL    0x02

/*
 * LCD 설정 블록 40byte (isldev/clsDevDeviceSetting.cs의 GetLCDInfo가 읽는 형식)
 *
 *   [0]      BackLightType            1  Default=1 / AlwaysOn=2 / Custom=3
 *   [1..2]   BackLight 시작 시각       2  바이트마다 16진 두 자리로 읽는다 -> BCD "HHMM"
 *   [3..4]   BackLight 종료 시각       2  (위와 같음)
 *   [5]      DateFormatType           1  yyyyMMdd=1 / MMddyyyy=2 / ddMMyyyy=3 / Custom
 *   [6..37]  DateFormatCustom         32 ASCII, 0으로 채움 (English/Korean일 때)
 *   [38..39] Reserved                 2
 *
 * 40byte보다 짧으면 DM이 null로 처리해 설정 조회가 실패로 뜬다.
 */
#define IDTI_LCD_INFO_LEN            40
#define IDTI_LCD_BACKLIGHT_DEFAULT   1
#define IDTI_LCD_DATEFORMAT_YYYYMMDD 1

/* clsDevParams.MultiLanguage. LCD가 없어도 언어 조회에는 값으로 답해야 한다(아래 net.c 참고) */
#define IDTI_LANGUAGE_ENGLISH 1

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
 *
 * **두 값은 서로 다른 enum이다** (`isldev/clsDevParams.cs`, 2026-09-10 확인):
 *
 *   Category = clsDevParams.DeviceType — 무엇의 부류인가
 *     None=0, Host=1, ComSlot=2, **Controller=3**, Module=4, Reader=5, OutPut=6, Input=7,
 *     Controller_Elv=19
 *
 *   DeviceType = clsDevParams.ControllerType — 어느 모델인가
 *     SSC_312=31, SSC_314_AL4=32, **SSC_324=33**, SSC_314_ECA=40,
 *     ISC_101=41, ISC_201=42, ISC_301=43, ISC_401=44, ISC_201A=45, ISC_201PK=46, ISC_101A=47,
 *     BSC_101=51 … BSC_101A=58
 *
 * 이전 값(Category=0, DeviceType=0x29)은 틀렸다. 0x29는 41 = **ISC_101**이고 Category 0은 None이다.
 * 아래는 기본값이고, 실제로 보고할 값은 config.json의 `device_category`/`device_type`으로 바꾼다
 * (등록하는 모델에 맞춰야 하므로 상수로 박아 두면 안 된다).
 */
#define IDTI_DEVICE_CATEGORY_CONTROLLER 3   /* DeviceType.Controller */
#define IDTI_CONTROLLER_TYPE_SSC_324    33  /* ControllerType.SSC_324 */
#define IDTI_CONTROLLER_TYPE_ISC_101    41  /* ControllerType.ISC_101 (이전에 쓰던 값) */

#define IDTI_DEVICE_CATEGORY_DEFAULT IDTI_DEVICE_CATEGORY_CONTROLLER
#define IDTI_DEVICE_TYPE_DEFAULT     IDTI_CONTROLLER_TYPE_SSC_324

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

/*
 * 시각 동기화 payload. FrameOption에 IDTI_FOPT_TIME_SYNC가 켜져 있으면 요청 Data에 실려 온다.
 * BCD 7byte: YY MM DD 요일 HH MM SS  (2026-09-10 13:19:30 목요일 -> 26 09 10 05 13 19 30)
 * 2026-09-10 실제 DM 패킷에서 확인했다. 요일은 우리가 쓰지 않는다.
 */
#define IDTI_TIME_SYNC_LEN 7

/*
 * Device Status V2의 모듈 구성 (근거: PC 소스 `isldev/clsDevStatus.cs`, 2026-09-10 확인)
 *
 *   [0]     DeviceCategory   1
 *   [1]     DeviceType       1
 *   [2..7]  DateTime         6 (BCD)
 *   [8..9]  IsExistModule    2   <- 모듈 존재 비트. **빅엔디안 16bit, 모듈 N = bit N (LSB가 모듈 0)**
 *   [10..]  모듈 14개 x 16byte = ModuleIOType(1) + ModuleInstallType(1) + IOStatus(14)
 *
 * 합계 10 + 14*16 = 234 = IDTI_DEVICE_STATUS_V2_LEN
 *
 * IOStatus 각 바이트는 **상위 니블 = IOType, 하위 니블 = IOStatus** 로 두 값을 담는다.
 */
#define IDTI_MODULE_COUNT_V2      14  /* clsDevStatus.countDeviceModule */
#define IDTI_MODULE_ENTRY_LEN     16  /* IOType 1 + InstallType 1 + IOStatus 14 */
#define IDTI_MODULE_IO_SLOTS      14  /* 모듈 하나가 담을 수 있는 I/O 개수 */
#define IDTI_MODULE_ARRAY_OFFSET  10  /* Device Status에서 모듈 배열이 시작하는 위치 */

/* clsDevParams.IOType */
#define IDTI_IOTYPE_NONE             0
#define IDTI_IOTYPE_TEMPLATE_READER  1
#define IDTI_IOTYPE_PROXIMITY_READER 2
#define IDTI_IOTYPE_INPUT_SENSOR     3
#define IDTI_IOTYPE_OUTPUT_RELAY     4

/* clsDevParams.IOStatus */
#define IDTI_IOSTATUS_NONE     0
#define IDTI_IOSTATUS_ACTIVE   1
#define IDTI_IOSTATUS_INACTIVE 2

/* clsDevParams.ModuleInstallType */
#define IDTI_MODULE_INSTALL_NONE     0
#define IDTI_MODULE_INSTALL_INTERNAL 1
#define IDTI_MODULE_INSTALL_EXTERNAL 2

/*
 * clsDevParams.ModuleType. 접미사 004/008/00C는 I/O 개수(4/8/12)로 보인다.
 * **우리 RRU가 이 중 무엇에 대응하는지는 아직 확정 전이라 config로 뺐다** (미확인 문서
 * `8. System Device Reader Setup` 확인 필요). 기본값은 8채널 계열로 잡았다.
 */
#define IDTI_MODULE_TYPE_RIM_008 92   /* Reader 계열 8채널 */
#define IDTI_MODULE_TYPE_ROM_008 102  /* Output 계열 8채널 */
#define IDTI_MODULE_TYPE_RRM_008 112
#define IDTI_MODULE_TYPE_RXM_132 123  /* 접미사가 (리더1, 입력3, 출력2)로 읽힌다 */

/*
 * 모듈 하나의 I/O 슬롯 구성 (2026-09-10 확정). 14슬롯을 꽉 채운다.
 *
 *   slot 0..1   카드리더 2
 *   slot 2..7   입력 6
 *   slot 8..11  출력 4
 *   slot 12..13 알람·화재 2 <- 모듈마다 실제로 배선된 입력이다 (SSC-324 실물: 모듈마다 알람 1 · 화재 1)
 *
 * 모듈 4개 -> 리더 8 / 입력 32 / 출력 16 으로 RRU 구성과 맞아떨어진다.
 */
#define IDTI_MODULE_DEFAULT_COUNT          4
#define IDTI_MODULE_DEFAULT_READERS        2
#define IDTI_MODULE_DEFAULT_INPUTS         6
#define IDTI_MODULE_DEFAULT_OUTPUTS        4
#define IDTI_MODULE_DEFAULT_ALARM_FIRE_INPUTS 2
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

/*
 * BCD 한 바이트를 정수로 되돌린다. 각 니블이 0~9가 아니면 -1.
 * (요청에 실려 오는 시각을 읽을 때 쓴다 - 상대가 보낸 값이라 검증이 필요하다)
 */
int idti_from_bcd(uint8_t value);

#endif /* ACU_PROTOCOL_H */
