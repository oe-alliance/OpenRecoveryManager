CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -Os -pipe
LDFLAGS ?=
LOCALEDIR ?= /usr/share/locale
PLUGINDIR ?= /usr/lib/enigma2/python/Plugins/SystemPlugins/RecoveryManager
REVISION ?= $(shell git rev-parse --short HEAD 2>/dev/null)

# LVGL is the submodule lib/lvgl, configured by include/lv_conf.h; qrcodegen.h is its copy of Nayuki's.
CPPFLAGS += -Iinclude -Ilib/lvgl -Ilib/lvgl/src/libs/qrcode -DLV_CONF_INCLUDE_SIMPLE -DLOCALEDIR='"$(LOCALEDIR)"' \
	-DORM_REVISION='"$(REVISION)"'
CFLAGS += -std=c11 -Wall -Wextra -Wformat=2 -Wshadow -Wpointer-arith
SECTIONS := -ffunction-sections -fdata-sections

PROGRAM := recovery-manager
SOURCES := \
	src/main.c \
	src/watch.c \
	src/remote.c \
	src/reset.c \
	src/flash.c \
	src/backup.c \
	src/boxinfo.c \
	src/crash.c \
	src/console.c \
	src/slots.c \
	src/plugins.c \
	src/update.c \
	src/about.c \
	src/language.c \
	src/i18n.c \
	src/viewer.c \
	src/ui.c \
	src/input.c \
	src/process.c
LIBRARY_SOURCES := $(wildcard src/fonts/*.c) $(shell find lib/lvgl/src -name '*.c')
OBJECTS := $(SOURCES:.c=.o)
LIBRARY_OBJECTS := $(LIBRARY_SOURCES:.c=.o)
DEPENDS := $(OBJECTS:.o=.d) $(LIBRARY_OBJECTS:.o=.d)
# The translations, po/<language>.po, installed as <language>/LC_MESSAGES/orm.mo.
LANGUAGES := $(basename $(notdir $(wildcard po/*.po)))
CATALOGS := $(LANGUAGES:%=po/%.mo)

.PHONY: all clean install pot

all: $(PROGRAM) $(CATALOGS)

$(PROGRAM): $(OBJECTS) $(LIBRARY_OBJECTS)
	$(CC) $(LDFLAGS) -Wl,--gc-sections -o $@ $(OBJECTS) $(LIBRARY_OBJECTS) -lm

# Without the warnings of ORM, in the C dialect of LVGL.
LIBRARY_CFLAGS = $(filter-out -std=% -W%,$(CFLAGS)) -std=gnu11

src/fonts/%.o: src/fonts/%.c
	$(CC) $(CPPFLAGS) $(LIBRARY_CFLAGS) $(SECTIONS) -MMD -MP -c -o $@ $<

lib/%.o: lib/%.c
	$(CC) $(CPPFLAGS) $(LIBRARY_CFLAGS) $(SECTIONS) -MMD -MP -c -o $@ $<

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SECTIONS) -MMD -MP -c -o $@ $<

po/%.mo: po/%.po
	msgfmt -c -o $@ $<

# The texts of the code for the translators, without line numbers, so only a changed text changes them.
pot:
	xgettext --from-code=UTF-8 --keyword=_ --keyword=N_ --keyword=ngettext:1,2 --add-comments=TRANSLATORS \
		--add-location=file --package-name=ORM --msgid-bugs-address= -o po/orm.pot $(SOURCES) include/licenses.h \
		plugin/*.py
	for po in po/*.po; do [ -f "$$po" ] && msgmerge -q -U --backup=none --add-location=file "$$po" po/orm.pot || true; done

install: $(PROGRAM) $(CATALOGS)
	install -d $(DESTDIR)/usr/bin
	install -m 0755 $(PROGRAM) $(DESTDIR)/usr/bin/$(PROGRAM)
	for language in $(LANGUAGES); do \
		install -d $(DESTDIR)$(LOCALEDIR)/$$language/LC_MESSAGES; \
		install -m 0644 po/$$language.mo $(DESTDIR)$(LOCALEDIR)/$$language/LC_MESSAGES/orm.mo; \
	done
	install -d $(DESTDIR)$(PLUGINDIR)
	install -m 0644 plugin/__init__.py plugin/plugin.py plugin/plugin.png plugin/plugin-fhd.png $(DESTDIR)$(PLUGINDIR)

clean:
	rm -f $(OBJECTS) $(LIBRARY_OBJECTS) $(DEPENDS) $(PROGRAM) $(CATALOGS)

-include $(wildcard $(DEPENDS))
