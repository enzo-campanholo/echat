.POSIX:
CC = cc
CFLAGS = -O2 -std=c89 -pedantic -Wall -Wextra -Werror
CPPFLAGS = -D_POSIX_C_SOURCE=200809L
LDFLAGS =
PREFIX = /usr/local
CORE = src/base.o src/store.o src/view.o src/engine.o
OBJ = $(CORE) src/pi.o src/main.o

all: unii

unii: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ)

src/base.o: src/base.c src/base.h
src/store.o: src/store.c src/store.h src/base.h
src/view.o: src/view.c src/store.h
src/engine.o: src/engine.c src/engine.h src/store.h
src/pi.o: src/pi.c src/pi.h src/base.h
src/main.o: src/main.c src/engine.h src/pi.h

.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

tests/base_test: tests/base_test.c src/base.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ tests/base_test.c src/base.o

tests/store_test: tests/store_test.c $(CORE)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ tests/store_test.c $(CORE)

tests/view_test: tests/view_test.c $(CORE)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ tests/view_test.c $(CORE)

tests/fake_pi: tests/fake_pi.c src/base.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ tests/fake_pi.c src/base.o

tests/server_test: tests/server_test.c src/base.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ tests/server_test.c src/base.o

tests/engine_test: tests/engine_test.c $(CORE)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ tests/engine_test.c $(CORE)

tests/pi_test: tests/pi_test.c src/pi.o src/base.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ tests/pi_test.c src/pi.o src/base.o

tests/fsync_test: tests/fsync_fault.c src/base.c src/store.c src/view.c src/engine.c src/pi.c src/main.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Dfsync=unii_test_fsync -Dfopen=unii_test_fopen -Isrc -o $@ tests/fsync_fault.c src/base.c src/store.c src/view.c src/engine.c src/pi.c src/main.c

test: all tests/base_test tests/store_test tests/view_test tests/fake_pi tests/fsync_test tests/server_test tests/engine_test tests/pi_test
	./tests/base_test
	./tests/store_test
	./tests/view_test
	./tests/engine_test
	./tests/pi_test
	sh tests/integration.sh

tests/pi_live_test: tests/pi_live_test.c src/base.o src/store.o src/view.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ tests/pi_live_test.c src/base.o src/store.o src/view.o

tests/pi_worker_test: tests/pi_worker_test.c src/base.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ tests/pi_worker_test.c src/base.o

tests/pi_failure_test: tests/pi_failure_test.c src/base.o src/store.o src/view.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ tests/pi_failure_test.c src/base.o src/store.o src/view.o

tests/pi_steering_test: tests/pi_steering_test.c src/base.o src/store.o src/view.o
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -Isrc -o $@ tests/pi_steering_test.c src/base.o src/store.o src/view.o

test-pi: all tests/fake_pi tests/pi_live_test tests/pi_worker_test tests/pi_failure_test tests/pi_steering_test
	sh tests/pi_live.sh
	sh tests/pi_worker.sh
	sh tests/pi_failure.sh
	sh tests/pi_steering.sh

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/unii/pi
	cp unii $(DESTDIR)$(PREFIX)/bin/unii
	cp pi/*.mjs pi/*.ts pi/prompt.txt $(DESTDIR)$(PREFIX)/share/unii/pi/

clean:
	rm -f unii src/*.o tests/base_test tests/store_test tests/view_test tests/fake_pi tests/fsync_test tests/server_test tests/engine_test tests/pi_test tests/pi_live_test tests/pi_worker_test tests/pi_failure_test tests/pi_steering_test
