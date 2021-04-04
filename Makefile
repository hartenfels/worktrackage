DEBUG_CFLAGS   := -g
RELEASE_CFLAGS := -O2 -DNDEBUG

CC      := gcc
CFLAGS  := -std=gnu11 -Wall -Wextra -Werror -pedantic -pedantic-errors
LDFLAGS := -lsqlite3 -lX11 -lXss

all: debug release

release: wtsnap

debug: wtsnap_debug

wtsnap: wtsnap.c Makefile
	$(CC) $(CFLAGS) $(RELEASE_CFLAGS) -o $@ $(LDFLAGS) $<

wtsnap_debug: wtsnap.c Makefile
	$(CC) $(CFLAGS) $(DEBUG_CFLAGS) -o $@ $(LDFLAGS) $<

install:
	@if [ -z "$$PREFIX" ]; then PREFIX='/usr/local/bin'; fi; \
		echo "Installing into '$$PREFIX'"; \
		cp -v wtsnap wtstats "$$PREFIX"

uninstall:
	@if [ -z "$$PREFIX" ]; then PREFIX='/usr/local/bin'; fi; \
		echo "Uninstalling from '$$PREFIX'"; \
		rm -vf "$$PREFIX/wtsnap" "$$PREFIX/wtstats"

clean:
	rm -f wtsnap wtsnap_debug

realclean: clean

.PHONY: all release debug install uninstall clean realclean
