.POSIX:
CC = cc
CFLAGS = -O2 -std=c89 -pedantic -Wall -Wextra -Werror
CPPFLAGS = -D_POSIX_C_SOURCE=200809L
LDFLAGS =
PREFIX = /usr/local
CORE = src/base.o src/store.o src/view.o src/engine.o
OBJ = $(CORE) src/main.o
TESTS = tests/base_test tests/store_test tests/view_test tests/engine_test \
	tests/server_test tests/fsync_test
PI_TESTS = tests/pi_live_test tests/pi_worker_test tests/pi_failure_test \
	tests/pi_steering_test tests/pi_bash_test

all: unii

unii: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ)

src/base.o: src/base.c src/base.h
src/store.o: src/store.c src/store.h src/base.h
src/view.o: src/view.c src/store.h src/base.h
src/engine.o: src/engine.c src/engine.h src/store.h src/base.h
src/main.o: src/main.c src/engine.h src/store.h src/base.h

.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

tests/base_test: tests/base_test.c src/base.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ tests/base_test.c src/base.o

tests/store_test: tests/store_test.c $(CORE)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ tests/store_test.c $(CORE)

tests/view_test: tests/view_test.c $(CORE)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ tests/view_test.c $(CORE)

tests/engine_test: tests/engine_test.c $(CORE)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ tests/engine_test.c $(CORE)

tests/server_test: tests/server_test.c src/base.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ tests/server_test.c src/base.o

# Every sync goes through the injected fsync. UNII_PLAIN_FSYNC skips
# F_FULLFSYNC, which succeeds first on macOS and would bypass the injection.
tests/fsync_test: tests/fsync_fault.c src/base.c src/store.c src/view.c src/engine.c src/main.c src/base.h src/store.h src/engine.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Dfsync=unii_test_fsync -DUNII_PLAIN_FSYNC -Isrc -o $@ tests/fsync_fault.c src/base.c src/store.c src/view.c src/engine.c src/main.c

test: all $(TESTS)
	./tests/base_test
	./tests/store_test
	./tests/view_test
	./tests/engine_test
	sh tests/integration.sh

tests/pi_live_test: tests/pi_live_test.c $(CORE)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ tests/pi_live_test.c $(CORE)

tests/pi_worker_test: tests/pi_worker_test.c $(CORE)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ tests/pi_worker_test.c $(CORE)

tests/pi_failure_test: tests/pi_failure_test.c $(CORE)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ tests/pi_failure_test.c $(CORE)

tests/pi_steering_test: tests/pi_steering_test.c $(CORE)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ tests/pi_steering_test.c $(CORE)

tests/pi_bash_test: tests/pi_bash_test.c $(CORE)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ tests/pi_bash_test.c $(CORE)

test-pi: all $(PI_TESTS)
	sh tests/pi_live.sh
	sh tests/pi_worker.sh
	sh tests/pi_failure.sh
	sh tests/pi_steering.sh
	sh tests/pi_bash.sh
	sh tests/pi_js.sh

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/unii/pi
	cp unii $(DESTDIR)$(PREFIX)/bin/unii
	cp pi/*.mjs pi/*.ts pi/prompt.txt $(DESTDIR)$(PREFIX)/share/unii/pi/

clean:
	rm -f unii src/*.o $(TESTS) $(PI_TESTS)
