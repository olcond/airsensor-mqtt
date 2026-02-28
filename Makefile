CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11
LDFLAGS = -lusb -lpaho-mqtt3c -lpthread

BIN     = airsensor
TEST    = tests/test_airsensor

.PHONY: all test clean

all: $(BIN)

$(BIN): airsensor.c
	$(CC) $(CFLAGS) -o $(BIN) airsensor.c $(LDFLAGS)

$(TEST): tests/test_airsensor.c
	$(CC) $(CFLAGS) -o $(TEST) tests/test_airsensor.c

test: $(TEST)
	./$(TEST)

clean:
	rm -f $(BIN) $(TEST)
