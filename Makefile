PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
SHAREDIR ?= $(PREFIX)/share/lodge

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
VERSION ?= $(shell cat VERSION)

# macOS: build a universal (Apple Silicon + Intel) binary so one package fits every Mac.
# Homebrew builds for the user's own machine and passes UNIVERSAL=0.
UNIVERSAL ?= 1
ifeq ($(shell uname -s),Darwin)
ifeq ($(UNIVERSAL),1)
CFLAGS += -arch arm64 -arch x86_64
endif
endif

BIN := lodge
SRC := src/lodge.c
INC := include
TEXTS := texts

# Windows: cross-compiled with mingw-w64 (macOS: brew install mingw-w64; Ubuntu: apt install mingw-w64).
WINCC ?= x86_64-w64-mingw32-gcc
WINDIR_OUT := build/windows/lodge
WINZIP := lodge-$(VERSION)-windows-x64.zip

.PHONY: all clean install uninstall pkg deb windows

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

# Windows archive: lodge-<version>-windows-x64.zip (lodge.exe with the pictures next to it)
windows:
	rm -rf build/windows "$(WINZIP)"
	mkdir -p "$(WINDIR_OUT)/texts"
	$(WINCC) -O2 -Wall -Wextra -static -DLODGE_VERSION=\"$(VERSION)\" -I$(INC) $(SRC) -o "$(WINDIR_OUT)/lodge.exe"
	cp -R "$(TEXTS)/." "$(WINDIR_OUT)/texts/"
	cp LICENSE "$(WINDIR_OUT)/"
	cd build/windows && zip -qr "../../$(WINZIP)" lodge

# Debian/Ubuntu package: lodge_<version>_<arch>.deb in the repository root
deb:
	./deb/build-deb.sh $(VERSION)

clean:
	rm -f "$(BIN)" lodge-*.pkg lodge-*-windows-x64.zip
	rm -rf build/windows
