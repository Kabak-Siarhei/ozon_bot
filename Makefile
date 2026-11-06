# Makefile — C-версия Telegram-бота (libcurl + SQLite + MuPDF).
#
# Linux (как в Docker):
#   make
#
# macOS с MuPDF из PyMuPDF (пример):
#   make MUPDF_INC=... MUPDF_LIBS=/path/libmupdf.dylib \
#        SYS_LIBS="-lpthread -lz" \
#        EXTRA_LIBS="-framework CoreGraphics -framework CoreText -framework CoreFoundation -undefined dynamic_lookup"

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2

MUPDF_INC  ?= /usr/local/include
MUPDF_LIB  ?= /usr/local/lib
MUPDF_LIBS ?= -lmupdf -lmupdf-third
SYS_LIBS   ?= -lpthread -ldl -lz -lcrypto
EXTRA_LIBS ?=

PKG_CFLAGS := $(shell pkg-config --cflags libcurl sqlite3 2>/dev/null)
PKG_LIBS   := $(shell pkg-config --libs libcurl sqlite3 2>/dev/null)

LDLIBS = $(MUPDF_LIBS) $(PKG_LIBS) -lm $(SYS_LIBS) $(EXTRA_LIBS)

BOT_SRCS = bot.c telegram.c db.c ui.c ttn_add_field.c ttn_config.c cJSON.c
CFLAGS_ALL = $(CFLAGS) -I. -I"$(MUPDF_INC)" $(PKG_CFLAGS)

.PHONY: all clean test test-core test-tg

all: ozon_bot

ozon_bot: $(BOT_SRCS) *.h
	$(CC) $(CFLAGS_ALL) -DTTN_NO_MAIN -o $@ $(BOT_SRCS) -L"$(MUPDF_LIB)" $(LDLIBS)

ttn_add_field: ttn_add_field.c ttn_config.c ttn_add_field.h ttn_config.h
	$(CC) $(CFLAGS_ALL) -o $@ ttn_add_field.c ttn_config.c -L"$(MUPDF_LIB)" $(LDLIBS)

test_core: tests/test_core.c db.c ui.c ttn_add_field.c ttn_config.c cJSON.c *.h
	$(CC) $(CFLAGS_ALL) -DTTN_NO_MAIN -o $@ tests/test_core.c db.c ui.c \
	    ttn_add_field.c ttn_config.c cJSON.c -L"$(MUPDF_LIB)" $(LDLIBS)

test_tg: tests/test_tg.c telegram.c cJSON.c *.h
	$(CC) $(CFLAGS_ALL) -o $@ tests/test_tg.c telegram.c cJSON.c \
	    $(PKG_LIBS) -lm $(SYS_LIBS) $(EXTRA_LIBS)

test-core: test_core
	./test_core

test-tg: test_tg
	chmod +x tests/run_tg_test.sh
	./tests/run_tg_test.sh

test: test-core test-tg

clean:
	rm -f ozon_bot ttn_add_field test_core test_tg
	rm -f /tmp/ozon_c_test.db /tmp/ozon_c_test_out.pdf
