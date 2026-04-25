PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SHAREDIR ?= $(PREFIX)/share/lodge

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

BIN := lodge
SRC := deb/src/lodge.c
INC := deb/include
TEXTS := deb/usr/games/lodge/texts

.PHONY: all clean install uninstall

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -I$(INC) $< -o $@

install: $(BIN)
	install -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(SHAREDIR)/texts"
	install -m 0755 "$(BIN)" "$(DESTDIR)$(BINDIR)/lodge-bin"
	install -m 0755 "scripts/lodge" "$(DESTDIR)$(BINDIR)/lodge"
	cp -R "$(TEXTS)/." "$(DESTDIR)$(SHAREDIR)/texts/"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/lodge" "$(DESTDIR)$(BINDIR)/lodge-bin"
	rm -rf "$(DESTDIR)$(SHAREDIR)"

clean:
	rm -f "$(BIN)"
