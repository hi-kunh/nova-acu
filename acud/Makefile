CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -O2
LDLIBS  = -lsqlite3 -lcjson
TARGET  = acud
SRC     = main.c log.c db.c events.c users.c access.c hal_mock.c config.c protocol.c loop.c net.c discover.c

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

# events.db 성능·링 삭제 시험 도구 (tools/events_bench.c)
bench: events_bench
events_bench: ../tools/events_bench.c events.c log.c
	$(CC) $(CFLAGS) -o events_bench ../tools/events_bench.c events.c log.c -lsqlite3

# users.db 등록·조회 성능 시험 도구 (tools/users_bench.c)
usersbench: users_bench
users_bench: ../tools/users_bench.c users.c log.c
	$(CC) $(CFLAGS) -o users_bench ../tools/users_bench.c users.c log.c -lsqlite3

# users.c 동작 확인 (tools/users_test.c)
userstest: users_test
users_test: ../tools/users_test.c users.c log.c
	$(CC) $(CFLAGS) -o users_test ../tools/users_test.c users.c log.c -lsqlite3

clean:
	rm -f $(TARGET) events_bench users_bench users_test

.PHONY: run bench usersbench userstest clean
