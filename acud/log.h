#ifndef ACU_LOG_H
#define ACU_LOG_H

/*
 * 로그 출력 대상 관리.
 *
 * 기본은 stdout이다. systemd로 띄우면 stdout이 journald로 들어가므로 그대로 두면 되고,
 * journald를 쓰지 않는 환경(직접 nohup 등)에서는 log_open()으로 파일을 지정한다.
 * 데몬화하면 터미널이 사라져 stdout 로그가 통째로 없어지기 때문에 필요한 장치다.
 */

/*
 * 로그 출력 파일을 연다(append). path가 NULL이거나 빈 문자열이면 stdout을 쓴다.
 * 파일 열기에 실패하면 stdout으로 되돌아가고 그 사실을 로그로 남긴다.
 */
void log_open(const char *path);

/*
 * 같은 경로로 로그 파일을 다시 연다. logrotate가 파일을 옮긴 뒤에도 계속 쓰려면 필요하다
 * (SIGHUP 처리 시 함께 호출한다). stdout을 쓰는 중이면 아무것도 하지 않는다.
 */
void log_reopen(void);

/* 로그 파일을 닫는다. stdout을 쓰는 중이면 아무것도 하지 않는다. */
void log_close(void);

/* 현재 로그 파일 경로를 반환한다. stdout을 쓰는 중이면 빈 문자열. */
const char *log_path(void);

/* 타임스탬프가 붙은 간단한 로그 출력 */
void log_msg(const char *msg);

#endif /* ACU_LOG_H */
