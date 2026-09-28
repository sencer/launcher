VERSION ?= 0.1.0
ARCH_FLAGS ?= -march=native
CC ?= gcc
CFLAGS ?= -O3 $(ARCH_FLAGS) -flto=auto -fno-plt -Wall -Wextra -Wno-maybe-uninitialized -I/usr/include/freetype2 -I/usr/include/libpng16
LDFLAGS ?= -Wl,-O1,--as-needed,-z,now -s
STATIC_LIBS = -L.libs -Wl,-Bstatic -lfreetype -lpng16 -lbz2 -lz -lbrotlidec -lbrotlicommon
DYNAMIC_LIBS = -Wl,-Bdynamic -lwayland-client -lxkbcommon -lm
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
WAYLAND_SCANNER ?= /usr/bin/wayland-scanner
WAYLAND_PROTOCOLS_DIR ?= /usr/share/wayland-protocols

TARGET = launcher
SRCS = main.c desktop_cache.c vim_input.c ui_render.c xdg-shell-protocol.c wlr-layer-shell-unstable-v1-protocol.c
OBJS = $(SRCS:.c=.o)
HEADERS = launcher.h nanosvg.h nanosvgrast.h xdg-shell-client-protocol.h wlr-layer-shell-unstable-v1-client-protocol.h

STATIC_LIB_NAMES = libfreetype libpng16 libbz2 libz libbrotlidec libbrotlicommon
STRIPPED_LIBS = $(patsubst %,.libs/%.a,$(STATIC_LIB_NAMES))

.PHONY: all clean protocols install

all: protocols $(TARGET)

protocols: xdg-shell-client-protocol.h xdg-shell-protocol.c \
           wlr-layer-shell-unstable-v1-client-protocol.h wlr-layer-shell-unstable-v1-protocol.c

xdg-shell-client-protocol.h: $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml
	$(WAYLAND_SCANNER) client-header $< $@

xdg-shell-protocol.c: $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml
	$(WAYLAND_SCANNER) private-code $< $@

wlr-layer-shell-unstable-v1-client-protocol.h: wlr-layer-shell-unstable-v1.xml
	$(WAYLAND_SCANNER) client-header $< $@

wlr-layer-shell-unstable-v1-protocol.c: wlr-layer-shell-unstable-v1.xml
	$(WAYLAND_SCANNER) private-code $< $@

.libs:
	mkdir -p .libs

.libs/%.a: | .libs
	@src=$$(ls /usr/lib/*-linux-gnu/$*.a 2>/dev/null || ls /usr/lib/$*.a 2>/dev/null); \
	cp "$$src" $@; \
	objcopy --remove-section=.sframe $@

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS) $(STRIPPED_LIBS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(STATIC_LIBS) $(DYNAMIC_LIBS)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -rf $(OBJS) $(TARGET) .libs
