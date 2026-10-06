# ORM - Open Recovery Manager

A native tool in the running image that steps in when Enigma2 does not start,
for example after a broken update. It needs neither Enigma2 nor Python.

## How it works

`enigma2.sh` starts `recovery-manager --watch` next to Enigma2. Enigma2 reports
its start to `/var/run/enigma2-orm.socket`:

| Message | When |
|---|---|
| `step <name>` | every start step of `eProfileWrite()`, one per plugin |
| `ready` | the main loop runs |
| `quit <code>` | Enigma2 ends on purpose, e.g. restart or shutdown |
| `crash <signal>` / `crash python` | from the crash handler, just before Enigma2 ends |

When Enigma2 ends before `ready` without `quit`, crashes within a minute after
it, or reports no new step for 120 seconds while starting, `enigma2.sh` shows
`recovery-manager --crash` instead of the blue screen. It tells where the start
stopped and starts Enigma2 again after a countdown of 15 seconds unless somebody
presses a key. A later crash shows the blue screen of Enigma2 as before.

`--watch` returns once the socket is there and prints the pid of the watch.
After Enigma2 ended, `--crash RESULT PID` ends the watch, which writes its
result, and decides from it whether the menu is needed. It answers like
Enigma2: 1 halts and 2 reboots the receiver. `recovery-manager --help` lists
all options.

The menu stays on the left, the right side shows what the chosen entry does.
UP and DOWN choose an entry, OK, RIGHT or its digit opens it on the right, whose
screens go back with BACK and to the menu from their first one; digits work only
in the menu. HELP chooses the language, BLUE opens Remote Support and INFO the
licenses of ORM over every screen, BACK returns to it.

ORM speaks the language of Enigma2: `config.osd.language`, else
`config.misc.locale`, else the default of the brand like Enigma2. HELP chooses
another one until ORM ends. A language needs its catalog and its locale, else
ORM stays English; the output of opkg, ofgwrite and the plugins stays as it is.

`recovery-manager` shows the same menu on request, after `init 4`; while
Enigma2 runs it only says so. From a shell, the first entry ends ORM, YELLOW
reboots and RED powers off. For SSH or support, `touch /etc/enigma2/.orm-once`
and a restart of Enigma2 open it once instead of Enigma2.

ORM logs every message of the socket with its time to `/tmp/orm.log`,
the log of the previous start is kept as `/tmp/orm.log.last`.

Remote Support runs `remotesupport start` of the RemoteSupport plugin on a
pseudo terminal. The menu shows its link as a QR code and asks every question
of the command line, such as the approval of a terminal, on the TV. BACK goes
back while the session keeps running in the background: its questions appear
over every screen, the header turns yellow with the users and terminals, and
Enigma2 takes the session over when it starts. OK ends the session.

The crash log view shows the newest crash log of Enigma2 and starts at the end
of its log, which the blue screen showed before. BLUE chooses another crash log,
the newest first, YELLOW shows the messages of the last start. A crash report is sent with
`crashreport` of the CrashReport plugin, run like `remotesupport`: the crash log
alone or with the debug log and diagnostics, confirmed on the TV. Its tracking
number and link are shown with a QR code.

Another slot of a multiboot receiver is booted with `multiboot-selector.sh` of
oe-alliance/MultiBootSelectorPlugin as it is. ORM takes `/tmp/multiboot-selector.sh`
first, so a supporter can replace it, then the one of the image, and otherwise
loads the script of the newest release from GitHub into `/tmp`. Its list shows
the slots with the running one chosen at first, OK restarts the receiver into
the chosen slot.

Disable plugins keeps Enigma2 from loading a plugin. When Enigma2 knows the
plugin blacklists (openatv/enigma2#3925), OK goes from enabled to disabled
temporarily (`/tmp/plugin_blacklist`, until the receiver restarts), to disabled
permanently (`/etc/enigma2/plugin_blacklist`) and back to enabled; nothing is
moved, so updates and removals of the plugin keep working. Without them the
plugin is moved from `/usr/lib/enigma2/python/Plugins` to
`/usr/lib/enigma2/python/Plugins.disabled` and back. Only plugins are listed,
a broken screen, tool, converter or renderer of Enigma2 is not fixed by
disabling it. A plugin that caused problems at the last start, as start step,
in the debug log or in the traceback of the crash log, is marked and chosen at
first.

Software update works like the online update of Enigma2: the traffic light of
the feed blocks a red feed, `opkg update`, the list of all updates with the
installed and the new version, one confirmation, then `opkg upgrade` with its
output as it comes. The packages Enigma2 leaves out, like busybox, are held.
The receiver is restarted afterwards in the menu.

Reset settings moves either the file `settings` or all of `/etc/enigma2` into
`/etc/enigma2-reset-<time>`, so nothing is deleted, and puts the defaults of
the image in place, but keeps the network. Enigma2 then starts with the wizard.
Only the skin keeps a copy of `settings` there, removes the GUI and display skin
from it and moves the user skins `skin_user*.xml` along, so Enigma2 starts with
its standard skin and all other settings.

Back up image saves the running slot like the image backup of Enigma2
(ImageBackup.py), without the USB recovery images, as
`<medium>/images/<distro>-<version>-<box>-backup-<time>_usb.zip`. The media are
the mounted, writable ones under `/media` with enough space, RED cancels the
backup and removes what was written.

Flash online/local flashes an image into the running slot like the
FlashManager of Enigma2: from the feed of the running distribution or others,
or a zip on the media, including the backups of ORM. Downloads are saved in
`images` of the medium, unzipped images of earlier flashes are removed first.
`ofgwrite -n` checks the image before anything is written, only then OK
flashes after a confirmation.

Long work shows a spinner with the time it runs; RED cancels it where that is
safe.

## Building

The interface is drawn with LVGL, the submodule `lib/lvgl`, configured by
`include/lv_conf.h`. Both need only libc.

```
git submodule update --init
make CC=<cross compiler>
```

The version is `ORM_VERSION` in `include/version.h`; a push of a new
version to master tags and releases it as `v<version>`.

The fonts in `src/fonts` are rendered by `tools/mkfonts.sh`, see
`tools/fonts/README.md`, and the licenses of the About screen in
`include/licenses.h` by `tools/mklicenses.py`.

## Translations

The texts are marked with `_()` and `N_()` for gettext. `make pot` collects them
into `po/orm.pot` and updates every `po/<language>.po`; `make` turns these into
catalogs, `make install` puts them into `/usr/share/locale/<language>/LC_MESSAGES/orm.mo`.
A new language is a new `po/<language>.po`, with its name in `src/language.c`.
On master a workflow updates `po/orm.pot` after every change of the sources;
the translations are made on Weblate, which merges the `.po` files with it.
The fonts hold Latin, modern Greek, Cyrillic, Arabic and Persian; a language written
from the right mirrors the screen.

## License

Copyright (C) 2026 OE-Alliance Open Recovery Manager contributors

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, version 3, see `LICENSE`.

It contains, under their own licenses, which the About screen (INFO) shows:

- LVGL (MIT, `lib/lvgl`) with the QR code generator of Project Nayuki (MIT).
- The font, rendered from DejaVu Sans, and icons of Font Awesome Free
  (SIL OFL 1.1), see `tools/fonts`.
