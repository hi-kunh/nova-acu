CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -O2
TARGET  = acud
SRC     = src/main.c

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: run clean
