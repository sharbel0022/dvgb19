# MSYS2/MinGW: använd -lpthread
# Linux/WSL:   byt till -pthread om du vill
CC      := gcc
CFLAGS  := -O2 -Wall -Wextra -pedantic -std=c11
LDFLAGS := -lpthread

BIN := pc

all: $(BIN)

$(BIN): producer_consumer.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f $(BIN)
