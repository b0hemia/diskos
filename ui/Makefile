# diskOS native UI (LVGL 9.2.2) for the FiiO Snowsky Disc.
# Requires a static mipsel-linux-musl cross toolchain and LVGL 9.2.2 (vendored in ./lvgl).
# Override paths for your environment on the command line, in the environment, or in a local
# (gitignored) config.mk, e.g.:
#   make CROSS=/opt/x-tools/mipsel-linux-musl/bin/mipsel-linux-musl-  LVGL=/path/to/lvgl
-include config.mk
APP   := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
NUI   ?= $(APP)
LVGL  ?= $(APP)/lvgl
CROSS ?= mipsel-linux-musl-
CC    := $(CROSS)gcc

CFLAGS := -Os -pipe -std=gnu11 -static -pthread -D_GNU_SOURCE -DLV_CONF_INCLUDE_SIMPLE \
  -ffunction-sections -fdata-sections \
  -I$(APP) -I$(NUI) -Wall -MMD -MP
# --gc-sections drops every provably-unreferenced function/data section at link (unused LVGL fonts,
# widgets, draw backends, sqlite fns) with NO behavior change - only unreachable symbols are removed.
LDFLAGS := -static -pthread -Wl,--gc-sections
LDLIBS  := -lm -lrt

LVGL_SRCS := $(shell find $(LVGL)/src -name "*.c")
APP_SRCS  := $(APP)/main.c $(APP)/sdio.c $(APP)/font_cjk_extra_16.c $(APP)/playstate.c $(APP)/fb_pan.c $(APP)/ipc.c $(APP)/ui.c $(APP)/art.c $(APP)/screenmgr.c $(APP)/home.c $(APP)/library.c $(APP)/musicdb.c $(APP)/settings.c $(APP)/config.c $(APP)/anim.c $(APP)/search.c $(APP)/saver.c $(APP)/quicksettings.c $(APP)/songinfo.c $(APP)/npmenus.c $(APP)/eqcustom.c $(APP)/apps.c $(APP)/kbinput.c $(APP)/playlistview.c $(APP)/folderbrowser.c $(APP)/books.c $(APP)/artcache.c $(APP)/colorpick.c $(APP)/albumwall.c $(APP)/debug_ui.c $(APP)/modes.c $(APP)/wifi.c $(APP)/bt.c $(APP)/weather.c $(APP)/lyrics.c $(APP)/toast.c $(APP)/lastfm.c $(APP)/lastfm_ui.c $(APP)/md5.c $(APP)/scanner.c $(APP)/fwcaps.c $(APP)/font_icons_28.c $(APP)/font_icons_20.c $(APP)/font_weather16.c $(APP)/font_intl_14.c $(APP)/font_intl_16.c $(APP)/font_intl_18.c $(APP)/font_intl_20.c
# bundled SQLite amalgamation (compiled in-process; replaces popen sqlite3 CLI)
SQLITE_FLAGS := -Os -pipe -static -ffunction-sections -fdata-sections -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION \
  -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DQS=0 -DSQLITE_OMIT_DEPRECATED
OBJS := $(LVGL_SRCS:.c=.o) $(APP_SRCS:.c=.o) $(APP)/sqlite3.o

mq_ui: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)
	$(CROSS)strip $@ || true
	@ls -l $@

$(APP)/sqlite3.o: $(APP)/sqlite3.c
	$(CC) $(SQLITE_FLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Boot-selector probe (installed read-only at /opt/diskos/bin/diskos-bootprobe by the installer).
diskos-bootprobe: $(APP)/tools/diskos_bootprobe.c
	$(CC) -Os -static -Wall -Wextra -Werror -o $@ $<
	$(CROSS)strip $@ || true
	@md5sum $@

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) mq_ui ipodos-ui diskos-bootprobe
-include $(OBJS:.o=.d)
