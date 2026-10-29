CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -pthread -O2
LDFLAGS ?= -pthread
TARGET  := deadlock_demo
OBJS    := main.o deadlock_guard.o

.PHONY: all clean run-naive run-guarded

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c deadlock_guard.h
	$(CC) $(CFLAGS) -c -o $@ $<

run-naive: $(TARGET)
	./$(TARGET) --naive

run-guarded: $(TARGET)
	./$(TARGET) --guarded

clean:
	rm -f $(TARGET) $(OBJS)
