PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SHAREDIR ?= $(PREFIX)/share/lodge

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
VERSION ?= $(shell cat VERSION)

# macOS: build a universal (Apple Silicon + Intel) binary so one package fits every Mac.
ifeq ($(shell uname -s),Darwin)
CFLAGS += -arch arm64 -arch x86_64
endif

BIN := lodge
SRC := src/lodge.c
INC := include
TEXTS := texts

.PHONY: all clean install uninstall pkg deb

all: $(BIN)

$(BIN): $(SRC) $(INC)/landscape.h VERSION
	$(CC) $(CFLAGS) -DLODGE_VERSION=\"$(VERSION)\" -I$(INC) $< -o $@

install: $(BIN)
	install -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(SHAREDIR)/texts"
	install -m 0755 "$(BIN)" "$(DESTDIR)$(BINDIR)/lodge-bin"
	install -m 0755 "scripts/lodge" "$(DESTDIR)$(BINDIR)/lodge"
	cp -R "$(TEXTS)/." "$(DESTDIR)$(SHAREDIR)/texts/"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/lodge" "$(DESTDIR)$(BINDIR)/lodge-bin"
	rm -rf "$(DESTDIR)$(SHAREDIR)"

# macOS installer: lodge-<version>.pkg
pkg:
	./macos/build-pkg.sh $(VERSION)

# Debian/Ubuntu package: lodge_<version>_<arch>.deb in the repository root
deb:
	./deb/build-deb.sh $(VERSION)

clean:
	rm -f "$(BIN)" lodge-*.pkg
