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
} AcuConfig;

/* 파일이 없거나 파싱에 실패해도 안전하게 쓸 수 있는 기본값을 채운다. */
void config_set_defaults(AcuConfig *cfg);

/*
 * path의 config.json을 읽어 cfg를 채운다.
 * 파일이 없거나 파싱 실패 시 cfg는 건드리지 않고 -1을 반환한다(호출자가 이전 값을 유지).
 * 성공하면 0을 반환한다.
 */
int config_load(const char *path, AcuConfig *cfg);

#endif /* ACU_CONFIG_H */
