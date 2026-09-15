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
#define IDTI_OBJ_HISTORY_COUNT 0x05 /* 이벤트 개수 */
#define IDTI_OBJ_HISTORY_INDEX 0x06 /* 이벤트 읽기 위치 (Restore Event log) */
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

/*
 * 사용자 파일(바이너리) 등록 결과 — `5. IDTi Protocol UserBinaryTransmit.doc` "Added Event Code".
 * DM이 "명단이 장비에 실제로 들어갔는가"를 아는 유일한 표식이다
 * (9/11 회신의 "DB 설정 ≠ 장비 설정" 문제와 같은 맥락).
 */
/*
 * 장비 자체 이벤트 (`dm/event_codes_all.csv` 확인값, 2026-09-11 Platinum 회신으로 용도 확정).
 * ⚠ **새 코드를 만들 수 없다** — 기존 IDTi 장비를 대체하는 것이라 DM·Platinum이 아는 코드만 쓴다.
 */
#define IDTI_EVENT_DOOR_FORCED_OPEN 0x18010119u /* Door Forced Open Mode */
#define IDTI_EVENT_DOOR_NORMAL      0x1801011Au /* Door Normal Mode */
#define IDTI_EVENT_ALARM_DETECTED   0x18010306u /* EX Sensor Motion Detected — 용도가 알람이면 이 코드 */
#define IDTI_EVENT_ALARM_RESTORED   0x18010307u /* EX Sensor Motion Restored */
#define IDTI_EVENT_FIRE_DETECTED    0x18010401u /* EM Sensor Fire Detected */
#define IDTI_EVENT_FIRE_RESTORED    0x18010402u /* EM Sensor Fire Restored */
#define IDTI_EVENT_HW_NO_RESPONSE   0x20030102u /* H/W No Response — RRU USB 단절 */

/*
 * 설정이 **장비에 실제로 들어갔다**는 표식 (DM 9/11 회신: "서버가 확인하는 유일한 표식").
 * 주소는 (모듈, **칸 번호 1부터**) — 현장 기록 `10210301 → (1,14)`, `10220301 → (1,9)(1,10)(2,11)`.
 */
/*
 * 이벤트 보관 명령 3종 — **SDK 소스(`isldev/clsDevCommand.cs`, `clsDevEvent.cs`)의 정의값**.
 *   EventCountCheck   Cmd 6 RequestData / Sub 2 Read   / Obj 0x05 HistoryCount
 *                     -> Data 36 = Count(4) + Reserve(32)
 *   EventIndexChange  Cmd 3 SendStatus  / Sub 5 Change / Obj 0x06 HistoryIndex
 *                     Data 36 = Reserve(1) + Type(1) + Offset(4) + StartDate(6) + EndDate(6) + Reserve(18)
 *                     -> Value(1)
 *   EventReset        Cmd 3 SendStatus  / Sub 6 Init   / Obj 0x01 History -> Value(1)
 *
 * DM 회신(9/14 6절): 현장 SSC-324가 **읽기 인덱스가 쓰기 위치보다 앞서** 이벤트를 하나도 못 올렸고,
 * 이 셋이 **없으면 현장에서 복구할 방법이 없다.** Index 타입 3 = 지정 시각부터, 4 = 시작~끝 구간.
 */
#define IDTI_EVENT_COUNT_LEN          36
#define IDTI_EVENT_INDEX_LEN          36
#define IDTI_EVENT_INDEX_OFF_TYPE      1
#define IDTI_EVENT_INDEX_OFF_OFFSET    2 /* 4 */
#define IDTI_EVENT_INDEX_OFF_START     6 /* 6, BCD YYMMDDhhmmss */
#define IDTI_EVENT_INDEX_OFF_END      12 /* 6 */
#define IDTI_EVENT_INDEX_TYPE_FROM_TIME 3
#define IDTI_EVENT_INDEX_TYPE_RANGE     4

#define IDTI_EVENT_INPUT_SET_OK  0x10210301u /* Data > Input  > Change > Success */
#define IDTI_EVENT_OUTPUT_SET_OK 0x10220301u /* Data > Output > Change > Success */

/*
 * 이벤트 주소 규칙 (2026-09-11 DM 회신, HARDWARE.md "이벤트 주소").
 * DM은 byte 6·7을 가공 없이 화면에 쓰므로 여기서 정확히 채워야 한다.
 */
#define IDTI_EVENT_ADDR_ALARM_READER 13 /* 알람 이벤트의 Reader 자리 (실측 0x01 0x0D) */
#define IDTI_EVENT_ADDR_NONE          0 /* 반응 장치가 리더가 아니거나 미설정일 때 */

/*
 * 강제 개방 (`13. IDTi Protocol Force OpenMode.doc`), Object 0xCE(206).
 *   설정  Cmd 0x03 SendStatus / Sub 0x05 Change / Obj 0xCE, Data(1) -> Result(1)
 *   조회  Cmd 0x06 RequestData / Sub 0x02 Read  / Obj 0xCE        -> Data(1)
 * Data는 **0x01이면 개방, 그 밖의 값이면 복구**다 (규약: "Open : 0x01  Recovery : Not 0x01").
 */
#define IDTI_OBJ_FORCE_OPEN 0xCE

/*
 * 장치 설정 오브젝트 — **DM 소스 `DeviceObject` 열거형의 정의값** (2026-09-14 DM 회신 2절).
 * 조회는 Cmd 6 RequestData / Sub 2 Read.
 */
#define IDTI_OBJ_DEVICE          0x29 /*  41 컨트롤러 기본설정 */
#define IDTI_OBJ_CONTROLLER      0x2B /*  43 컨트롤러 장치설정 */
#define IDTI_OBJ_INPUT           0x2C /*  44 입력 */
#define IDTI_OBJ_OUTPUT          0x2D /*  45 출력 */
#define IDTI_OBJ_CARD_READER     0x2F /*  47 카드리더 */
#define IDTI_OBJ_ALARM           0x51 /*  81 알람 */
#define IDTI_OBJ_ALARM_BELL_SCH  0x52 /*  82 알람벨 스케줄 */
#define IDTI_OBJ_OPMODE_SCH      0x53 /*  83 운영모드 스케줄 */
#define IDTI_OBJ_DOORMODE_SCH    0x54 /*  84 도어모드 스케줄 */
#define IDTI_OBJ_GROUP_APPLY     0xA6 /* 166 엑세스그룹 적용 (0x2F와 헷갈렸던 번호) */

/* 설정 데이터 블록 크기 — SSC-324 캡처의 OneDataBlockSize (DM 회신 3절) */
#define IDTI_DEVICE_BASIS_LEN  13 /* 0x29 */
#define IDTI_INPUT_SETTING_LEN 21 /* 0x2C */
#define IDTI_OUTPUT_SETTING_LEN 14 /* 0x2D */

/*
 * 컨트롤러 기본설정 13byte 배치 (DM 회신 3-1, SSC-324 캡처 `03 21 01 05 FF 00 00 00 00 00 00 00 00`).
 * Platinum이 보여 주던 `1/5/0`은 Address / OperationMode / Floor였다.
 */
#define IDTI_BASIS_OFF_CATEGORY   0
#define IDTI_BASIS_OFF_TYPE       1
#define IDTI_BASIS_OFF_ADDRESS    2
#define IDTI_BASIS_OFF_OPMODE     3
#define IDTI_BASIS_OFF_LEVEL      4
#define IDTI_BASIS_OFF_VALIDATION 5 /* 2 */
#define IDTI_BASIS_OFF_FLOOR      7 /* 2 */

/*
 * 출력 설정 14byte 배치 (DM 회신 3-3). 알람·화재 릴레이 규칙이 이 두 칸을 본다.
 *   ActiveType     2 = Alarm인 출력은 보드 전체에서 전부 동작
 *   IsActiveOption bit7 = 화재로도 동작, bit6 = 협박으로 동작
 */
#define IDTI_OUTPUT_OFF_ACTIVE_TYPE   6
#define IDTI_OUTPUT_OFF_ACTIVE_TIME   7
#define IDTI_OUTPUT_OFF_ACTIVE_OPTION 10
#define IDTI_OUTPUT_ACTIVE_TYPE_DOOR  1
#define IDTI_OUTPUT_ACTIVE_TYPE_ALARM 2
#define IDTI_OUTPUT_OPTION_BY_FIRE    0x80
#define IDTI_OUTPUT_OPTION_BY_DURESS  0x40

/*
 * 요청 헤더에서 **"어느 칸을 묻는가"** 를 꺼낸다 (DM 회신 4절).
 *
 *   offset  7~10  AddressDest           마지막 byte(offset 10) = 모듈 번호 (0이면 컨트롤러 자신)
 *   offset 11~14  AddressDestBroadcast  32비트 비트맵, 켜진 비트 = 대상
 *
 * `StartDataBlockIdx`(36~37)는 "몇 개짜리 묶음인가"만 말하고 **몇 번 칸인지는 말하지 않는다.**
 * IdtiHeader.dest_addr[8]이 offset 7~14를 그대로 담고 있다.
 */
/*
 * **칸 번호 두 가지** — 섞지 말 것.
 *   비트 번호(0부터)  요청 비트맵의 비트 = 14칸 배치의 자리. 저장 열쇠로 쓴다
 *   칸 번호(1부터)    이벤트 주소·반응 장치 주소 = 비트 번호 + 1
 * 현장 기록이 1부터를 보여 준다: 알람 `(1,13)` = 자리 12, 출력 설정 `(1,9)` = 자리 8(첫 출력).
 */
#define IDTI_SLOT_EVENT_BASE 1

#define IDTI_DEST_MODULE_IDX 3 /* dest_addr[3] = offset 10 */
#define IDTI_DEST_BITMAP_IDX 4 /* dest_addr[4..7] = offset 11~14, 빅엔디언 */
#define IDTI_FORCE_OPEN_ON  0x01

#define IDTI_EVENT_USERFILE_PARTIAL 0x101D0101u /* 일부 사용자가 등록되지 않음 */
#define IDTI_EVENT_USERFILE_SUCCESS 0x101D0102u /* 전부 등록 성공 */
#define IDTI_EVENT_USERFILE_FAIL    0x101D0103u /* 전부 실패 */

/*
 * 사용자 바이너리 전송 (`5.` 문서). PC -> 장치.
 *   Start    Cmd 0x05 / Sub 0x03 / Obj 0xD0, Data(10) = 총 크기(4) + 총 인원(4) + OBJ(1) + Rev(1)
 *   Continue Cmd 0x05 / Sub 0x03 / Obj 0xD1, Data(2+N) = Index(2) + 원시 데이터 N
 * 응답은 Data(1) = Success(1) / Fail(2). Fail을 받으면 PC가 **직전 패킷을 다시 보낸다.**
 */
/*
 * 사용자 오브젝트 (`1.` 문서 Object Table, `3.` 문서 2절 Transmit Structure).
 * 어느 것으로 올지는 DM이 무엇을 담아 보내느냐에 달렸다 — 데이터 길이로 구분한다.
 */
#define IDTI_OBJ_USER_ALL        0x15 /* Info + Name + Card + Finger */
#define IDTI_OBJ_USER_INFO       0x16 /* Info + Name */
#define IDTI_OBJ_USER_CARD       0x17 /* Info + Card + Name */
#define IDTI_OBJ_USER_FINGER     0x18 /* Info + Finger */
#define IDTI_OBJ_USER_RESTRICT   0x20 /* Info + Restriction */
#define IDTI_OBJ_USER_DATA       0x21 /* Info + Name + Card + Restriction + Group = 112byte */
#define IDTI_OBJ_USER_DATA_FGR   0x22 /* 위 + Finger */
#define IDTI_OBJ_USER_IMAGE      0x23 /* Info + Image */

/*
 * 1명씩 주고받는 명령 (`1.` 문서).
 *   전송  Cmd 0x05 / Sub 0x03 / Obj 사용자 오브젝트 + Data(N)
 *   삭제  Cmd 0x05 / Sub 0x04 / Obj 사용자 오브젝트 + Data(12) = User ID(8) + Revision(4)
 *   받기  Cmd 0x06 / Sub 0x02 / Obj 사용자 오브젝트 + Data(12)
 * 응답은 전송·삭제가 Data(1) = Result, 받기는 해당 오브젝트의 데이터.
 */
#define IDTI_USERCMD_KEY_LEN 12 /* User ID(8) + Revision ID(4) */

/* UserData(0x21) 한 명의 길이와 배치 (`1.` 문서 예시, `3.` 문서 2절) */
#define IDTI_USERDATA_LEN          112
#define IDTI_USERDATA_OFF_INFO       0
#define IDTI_USERDATA_OFF_NAME      32
#define IDTI_USERDATA_OFF_CARD      48
#define IDTI_USERDATA_LEN_CARD      32
#define IDTI_USERDATA_OFF_RESTRICT  80
#define IDTI_USERDATA_LEN_RESTRICT  16
#define IDTI_USERDATA_OFF_GROUP     96

#define IDTI_OBJ_USERBIN_START    0xD0
#define IDTI_OBJ_USERBIN_CONTINUE 0xD1

/* Binary OBJ (Start의 Data 9번째 byte) */
#define IDTI_USERBIN_OBJ_INFO        0x01 /* _SSCUserInfo       128byte */
#define IDTI_USERBIN_OBJ_INFO_FINGER 0x02 /* _SSCUserInfoFinger 1808byte (128 + 420*4) */

/*
 * `_SSCUserInfo` 128byte — **SSC-314/324가 쓰는 구조**다 (`5.` 문서).
 * 우리는 SSC-324 대체이므로 이것이 기준이다.
 *
 *   0   flag[4]      사용자 인덱스
 *   4   serial[4]    사용자 인덱스
 *   8   user[32]     User Info (규약 A절)
 *   40  card[12]     User Card — **앞 8byte가 뒤집혀 실린다** (아래 주의)
 *   52  name[16]     LCD 표시 이름
 *   68  restrict[8]  Restriction (단일 사용자 경로에서는 16byte)
 *   76  grpcode[16]  그룹 코드 2 x 8
 *   92  apb[1]       0x00
 *   93  reserved[33] 0x00
 *   126 crc_calc[1]  0x01
 *   127 datacrc[1]   user[0]부터 crc_calc까지의 XOR
 *
 * ⚠ **카드 바이트 뒤집기** — `clsDevUserBin.cs`에 이렇게 적혀 있다:
 *   "실제 사용 바이트는 8 바이트 이고, 단말기의 바이너리 전송 처리를 위해
 *    카드정보 송/수신시 꼬았던 바이트를 원래대로 복구시켜서 보내야 함"
 * 코드도 `Array.Copy(8) -> Array.Reverse -> Prox32[0..7]`이다.
 * 즉 **바이너리의 card[0..7]을 뒤집으면 단일 사용자 경로의 Proximity Data 앞 8byte**가 된다.
 */
#define IDTI_USERBIN_REC_INFO_LEN        128
#define IDTI_USERBIN_REC_INFO_FINGER_LEN 1808

#define IDTI_USERBIN_OFF_USER      8
#define IDTI_USERBIN_OFF_CARD      40
#define IDTI_USERBIN_LEN_CARD      12
#define IDTI_USERBIN_OFF_NAME      52
#define IDTI_USERBIN_OFF_RESTRICT  68
#define IDTI_USERBIN_LEN_RESTRICT  8
#define IDTI_USERBIN_OFF_GROUP     76
#define IDTI_USERBIN_OFF_CRC_CALC  126
#define IDTI_USERBIN_OFF_DATACRC   127

/* User Info 32byte 안의 배치 (`3.` 문서 A절, isldev clsDevUser.cs) */
#define IDTI_USERINFO_LEN            32
#define IDTI_USERINFO_OFF_ID          0
#define IDTI_USERINFO_OFF_GENGROUP    8
#define IDTI_USERINFO_OFF_RESERVED    9
#define IDTI_USERINFO_OFF_REVISION   10
#define IDTI_USERINFO_OFF_OPTION     12
#define IDTI_USERINFO_OFF_LEVEL      16
#define IDTI_USERINFO_OFF_VALIDATION 17
#define IDTI_USERINFO_OFF_TIMEZONE   19
#define IDTI_USERINFO_OFF_EXPIRED    21
#define IDTI_USERINFO_OFF_PROXTYPE   24
#define IDTI_USERINFO_OFF_PROXWIEG   25
#define IDTI_USERINFO_OFF_BIOTYPE    26
#define IDTI_USERINFO_OFF_BIOSUB     27
#define IDTI_USERINFO_OFF_TMPLCOUNT  28
#define IDTI_USERINFO_OFF_PASSWORD   29
#define IDTI_USERINFO_OFF_CANTEEN    31

/* Operation Mode (Event Info 내) */
#define IDTI_OPMODE_CARD 0x02
#define IDTI_OPMODE_NONE 0x00 /* 카드 태그가 아닌 장비 자체 이벤트 (파일 등록 결과 등) */

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
 * 값은 config로 뺄 수 있게 두되, **기본값은 현장 SSC-324와 같은 61/INTERNAL** 로 맞춘다
 * (2026-09-11 DM 회신 6-3: ACU03·ACU04의 devmodule1·2가 modeltype 61, installedtype 1).
 * 우리는 그 장비를 대체하는 것이므로 서버 화면에 같은 종류로 보여야 한다.
 */
#define IDTI_MODULE_TYPE_SSC_324 61   /* 현장 SSC-324 실측값 — NCU 기본값 */
#define IDTI_MODULE_TYPE_RIM_008 92   /* Reader 계열 8채널 */
#define IDTI_MODULE_TYPE_ROM_008 102  /* Output 계열 8채널 */
#define IDTI_MODULE_TYPE_RRM_008 112
#define IDTI_MODULE_TYPE_RXM_132 123  /* 접미사가 (리더1, 입력3, 출력2)로 읽힌다 */

/*
 * 이벤트 정보 블록의 주소 두 칸(byte 6 Module / byte 7 Reader)은 **1부터** 센다.
 * DM 파서는 ModuleAddress = bEvent[6], ReaderAddress = bEvent[7]로 원시값을 그대로 쓴다
 * (2026-09-11 DM 회신). 0으로 보내면 서버 화면에서 어느 리더인지 알 수 없다.
 */
#define IDTI_EVENT_ADDR_FIRST_MODULE 1
#define IDTI_EVENT_ADDR_FIRST_READER 1

/*
 * 모듈 하나의 I/O 슬롯 구성 (2026-09-10 확정). 14슬롯을 꽉 채운다.
 *
 *   slot 0..1   카드리더 2
 *   slot 2..7   입력 6
 *   slot 8..11  출력 4
 *   slot 12..13 알람·화재 2 <- 모듈마다 실제로 배선된 입력이다 (SSC-324 실물: 모듈마다 알람 1 · 화재 1)
 *
 * 기본 모듈 2개 -> 리더 4 / 입력 16 / 출력 8. 개발 우선 대상 RRU-M2 한 대이자
 * SSC-324 본체와 같은 모양이다 (2026-09-11 변경, 이전 기본값 4).
 */
#define IDTI_MODULE_DEFAULT_COUNT          2
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
