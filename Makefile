CC = cc
CPPFLAGS = -Iinclude
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
LDLIBS = -lm -pthread
CORE = src/cards.c src/range.c src/engine.c src/profile.c src/storage.c src/odds.c src/exact.c
COMMANDS = src/catalog.c src/table.c src/train.c src/model.c

.PHONY: all test sanitize clean
all: build/poker

build:
	mkdir -p build

build/poker: $(CORE) $(COMMANDS) src/main.c include/poker.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) $(COMMANDS) src/main.c $(LDFLAGS) $(LDLIBS) -o $@

build/test_poker: $(CORE) tests/test_poker.c include/poker.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/test_poker.c $(LDFLAGS) $(LDLIBS) -o $@

build/test_storage: $(CORE) tests/test_storage.c include/poker.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/test_storage.c $(LDFLAGS) $(LDLIBS) -o $@

build/test_odds: $(CORE) tests/test_odds.c include/poker.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/test_odds.c $(LDFLAGS) $(LDLIBS) -o $@

build/test_exact: $(CORE) tests/test_exact.c include/poker.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/test_exact.c $(LDFLAGS) $(LDLIBS) -o $@

test: build/poker build/test_poker build/test_storage build/test_odds build/test_exact
	./build/test_poker
	./build/test_storage
	./build/test_odds
	./build/test_exact
	sh tests/cli.sh ./build/poker
	sh tests/features.sh ./build/poker
	sh tests/training.sh ./build/poker
	sh tests/model.sh ./build/poker

sanitize:
	$(MAKE) clean
	$(MAKE) test CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined'

clean:
	rm -rf build
