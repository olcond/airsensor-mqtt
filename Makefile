CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11
LDFLAGS = -lusb-1.0 -lpaho-mqtt3cs -lpthread -lssl -lcrypto

BIN     = airsensor
TEST    = tests/test_airsensor

.PHONY: all test clean

all: $(BIN)

$(BIN): airsensor.c airsensor.h
	$(CC) $(CFLAGS) -o $(BIN) airsensor.c $(LDFLAGS)

$(TEST): tests/test_airsensor.c airsensor.h
	$(CC) $(CFLAGS) -o $(TEST) tests/test_airsensor.c

test: $(TEST)
	./$(TEST)

clean:
	rm -f $(BIN) $(TEST)
