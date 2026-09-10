#ifndef ACU_CONFIG_H
#define ACU_CONFIG_H

/*
 * config.json 설정 값 (3단계: config.json 감시 + 무중단 리로드).
 * SIGHUP을 받으면 config_load()를 다시 호출해 이 구조체를 갱신하고,
 * 데몬을 재시작하지 않고도 새 설정을 반영한다.
 */
typedef struct {
    char db_path[256];       /* SQLite3 DB 파일 경로 */
    int  door_open_seconds;  /* 도어 릴레이 동작 시간(초). IDTi Relay ActiveTime(1~99) 대응 */
    char admin_password[5];  /* 단말기(관리자) 비밀번호, 숫자 4자리 문자열. 웹 설정 화면 로그인에 사용.
                               * IDTi Header의 Password(4byte)/User Info의 Password(2byte BCD) 개념과
                               * 같은 맥락 - 5단계 네트워크 프로토콜 구현 시 재사용/정합성 검토 예정 */
    int  tcp_port;            /* IDTi 프로토콜 V2 TCP 서버 포트 (5단계, 1~65535) */
    char pid_path[256];      /* PID 파일 경로. 웹 설정 화면이 SIGHUP을 보낼 대상을 찾는 데 쓴다 */
    char log_path[256];      /* 로그 파일 경로. 빈 문자열이면 stdout (systemd에서는 journald로 들어감) */
    char net_iface[16];      /* UDP 탐색 응답에 실을 네트워크 인터페이스 (MAC/IP/넷마스크를 여기서 읽는다) */
    char netcfg_request_path[256]; /* UDP 탐색의 SETT를 받아 적을 파일. root 쪽 acu-netcfg-apply가 집어 간다.
                                    * 빈 문자열이면 SETT를 거절한다 */
    int  device_category;    /* Device Status/Firmware의 Category. clsDevParams.DeviceType 값.
                              * Controller=3 (protocol.h의 IDTI_DEVICE_CATEGORY_* 참고) */
    int  device_type;        /* 같은 곳의 DeviceType. clsDevParams.ControllerType 값.
                              * SSC_324=33, ISC_101=41 등. **PC에 등록한 모델과 맞아야 한다** */
    int  time_sync_enabled;  /* 상위 시스템이 보내는 시각으로 시계를 맞출지 (1=켬).
                              * 고립망에는 NTP가 없을 수 있어 기본은 켬 */
    int  inactivity_seconds; /* 상위 시스템 연결의 유휴 타임아웃(초). netmodule InactivityTime.
                              * 0이면 끔. 2byte 필드라 0~65535. DM 기본값은 600(10분) */
    char discovery_sett_password[5]; /* UDP 탐색 SETT에 요구할 비밀번호(4자리).
                                      * 빈 문자열이면 요구하지 않는다 - 기존 IntelliScan Device Manager는
                                      * 비밀번호를 보낼 수단이 없어서, 요구하면 SETT가 통하지 않는다 */
} AcuConfig;

/*
 * pid_path / log_path / net_iface / netcfg_request_path / discovery_sett_password는
 * config.json에 없어도 된다(선택 필드).
 * 없으면 기본값으로 되돌아가므로, 키를 지우는 것으로 기본 동작을 복구할 수 있다.
 * 나머지 필드는 하나라도 없거나 잘못되면 config_load()가 실패하고 cfg를 건드리지 않는다.
 */

/* 파일이 없거나 파싱에 실패해도 안전하게 쓸 수 있는 기본값을 채운다. */
void config_set_defaults(AcuConfig *cfg);

/*
 * path의 config.json을 읽어 cfg를 채운다.
 * 파일이 없거나 파싱 실패 시 cfg는 건드리지 않고 -1을 반환한다(호출자가 이전 값을 유지).
 * 성공하면 0을 반환한다.
 */
int config_load(const char *path, AcuConfig *cfg);

#endif /* ACU_CONFIG_H */
