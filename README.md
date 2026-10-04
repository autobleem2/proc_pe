# proc_pe

An AutoBleem **scanner processor** that turns PE app packages (`.mod` files) into AutoBleem Apps. A user drops a
`.mod` in the `Mods/` folder; before the next scan this processor unpacks it, and each program inside shows up
as an App (`Apps/pe-<name>/`, category "PE"). It is modelled on [proc_unzip](https://github.com/autobleem2/proc_unzip):
one C++ file, [`src/main.cpp`](src/main.cpp), nothing but the standard library and the vendored decoders
([libarchive](third_party/libarchive) with [liblzma](third_party/xz) for the tar.xz data, [miniz](third_party/miniz)
for a gzip'd control file). The `.mod` stays in `Mods/`; the package is never executed by this program.

## Installing

```
System/Processors/pe/
    processor.ini
    bin/psc/pe                     <- the PlayStation Classic (the packages are for it)
    bin/linux-armhf/pe  bin/linux-arm64/pe  bin/linux-i386/pe  bin/windows-x86_64/pe.exe   <- builds for tests
```

It is `Kinds=mods`: the launcher's scan starts it for the `Mods/` folder (`--start --mods`, with `AB_APPS_DIR` for
the Apps folder). It is `Default=on`: the launcher switches it on the first time it meets it (a player who switches
it off in Scanner processors keeps it off). The compatibility list is `rc/pe_compat.ini` of the launcher's package; without it the copy
built into the program is used.

## What it makes

A package is a Debian archive (`ar`: `debian-binary`, `control.tar.gz|xz`, `data.tar.xz`). For every launcher
folder `media/project_eris/etc/project_eris/SUP/launchers/<dir>/` that has a `launcher.cfg` and a `launch.sh` and
that the compatibility list allows, `Apps/pe-<launcher_filename>/` holds:

- the folder, **byte for byte** as the package has it (modes kept; a link inside it becomes a copy - the stick is FAT);
- `app.ini`:
  ```
  Title=<launcher_title>
  Author=<launcher_publisher, else the maintainer's name (no address), else empty>
  Version=<control Version>
  Image=<launcher_filename>.png        (only if the file exists)
  Readme=readme.txt
  Startup=run.sh
  Exec.psc=run.sh                      (so only the console lists it)
  Category=PE
  PeSource=<the .mod file name>
  PadMode=<pad= of the launcher's section in pe_compat.ini: psc, x360, psc-kernel or x360-kernel; psc-kernel without it>
  (an unknown pad value is a #WARN and psc-kernel; an unchanged package keeps its App, a changed one gets the current value)
  Dpad2Analog=<dpad2analog= of that section, 1 or 0; no line without it>
  Analog2Dpad=<analog2dpad= of that section, 1 or 0; no line without it>
  ```
- `readme.txt`: the control Description (its free-text lines, not its metadata), "Put the files this program needs
  (game data) in this folder.", and a one-line compatibility note - never the package's own README;
- `run.sh` (executable): `exec sh "${AB_RC_DIR:-/media/Autobleem/rc}/pe_run.sh" "$APP_DIR"`.

`launcher.cfg` is read as data (`name="value"` lines, CRLF, quotes of both kinds, comments) and never run. The
product's old name in any shown text is written as "PE".

The compatibility list (`[section]`, `block=1` or `skip=1`, `reason=`; a section is a `launcher_filename`, or the
folder's name) refuses a launcher with a `#WARN` and the reason; the rest of the package is still converted.

## The rules it keeps

- **atomic**: everything is unpacked into `Apps/.pe_tmp/` (same filesystem) and a folder is renamed into `Apps/`
  only when complete. A killed run is cleared at the next start; a folder a killed replacement had moved aside is
  put back.
- **idempotent**: a package already converted (its marker in `Apps/.pe_state/`, same size and Version, its Apps
  there with the same `PeSource` and `Version`) is not even unpacked.
- **never overwrites what is not its own**: an `Apps/pe-*` folder without `PeSource` stays; the same or an older
  Version than the installed one changes nothing (an older one warns); a **newer Version replaces the folder and
  keeps every file of the old folder that the new package does not ship**, at any depth (saves, game data - and
  also a file the old version shipped and the new one dropped).
- **safe**: only the launcher folders are unpacked. A name with `..`, an absolute name, a backslash or a drive
  colon anywhere in the package refuses the whole package; a link must stay inside its own launcher folder;
  a file is at most 2 GB, a package 3 GB, 50 000 entries.
- a package that is damaged, unsafe, not of type `USB_MOD` or not an archive is a `#WARN` and at the end
  `#ERROR - n of m packages could not be converted` (exit 1); a refused launcher is only a `#WARN`.

## The protocol

```
pe --version                                  #PE app packages V1.0.0 - Turns PE app packages (.mod) into Apps
pe --ismine --mod <file>                      exit 0 = a .mod that is an ar archive, 1 = not mine
pe --start --mods <Mods dir> [--apps <dir>] [--compat <file>]
pe --start --mod <file> [--apps <dir>] [--compat <file>]
```

The Apps folder is `--apps`, else `$AB_APPS_DIR`, else `Apps/` next to the Mods folder. The list is `--compat`,
else `$AB_PE_COMPAT`, else `rc/pe_compat.ini` under `$AB_ROOT` (or `$AB_ROOT/Autobleem`), else the built-in copy.
It prints, flushed line by line: `#Starting - <title>`, `#Converting <file>`, `n/m`, `0`..`100`, `#Adding <title>`,
`#WARN - <text>`, then `#DONE` (exit 0) or `#ERROR - <text>` (exit 1).

## Building and testing

- Every target as CI does: `ci/build.sh all` in the `ghcr.io/autobleem2/autobleem-build:develop` image
  (`dist/pe-<version>.zip`). Windows by hand: `./make_win.sh` (MSYS2 UCRT64).
- Anything else: `cmake -S . -B build && cmake --build build && (cd build && ctest)`.
- The self-test (`tests/selftest.cpp`) runs the real program on the packages in `tests/data/`, made by
  `tests/make_fixtures.py` (Python standard library, so unsafe ones can be made too) and `tests/make_dpkg_fixture.sh`
  (the real `dpkg-deb`).

GPL-3.0-or-later (`LICENSE`); miniz is MIT, libarchive BSD-2-Clause, liblzma 0BSD (`third_party/*`).
