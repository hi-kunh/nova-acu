CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -O2
LDLIBS  = -lsqlite3 -lcjson
TARGET  = acud
SRC     = main.c log.c db.c access.c hal_mock.c config.c

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDLIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: run clean
