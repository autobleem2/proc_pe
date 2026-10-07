# proc_pe

An AutoBleem **scanner processor** that turns PE app packages (`.mod` files) into AutoBleem Apps. A user drops a
`.mod` in the `Mods/` folder; before the next scan this processor unpacks it, and each program inside shows up
as an App (`Apps/pe-<name>/`, category "PE"). It is modelled on [proc_unzip](https://github.com/autobleem2/proc_unzip):
one C++ file, [`src/main.cpp`](src/main.cpp), nothing but the standard library and the vendored decoders
([libarchive](third_party/libarchive) with [liblzma](third_party/xz) for the tar.xz data, [miniz](third_party/miniz)
for a gzip'd control file). Once a package is converted the `.mod` moves to `Mods/done/` (see "The rules it keeps");
the package is never executed by this program.

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
  Exec.psc=run.sh                      (so only the console lists it; Exec.rpi=run.sh for a package whose control file says
                                        "Platform: RPI armhf" - only the Raspberry Pi 32-bit lists that one; Exec.rpi64=run.sh
                                        for "RPI64 arm64" (the 64-bit Pi), Exec.pcusb=run.sh for "PCUSB i386" (the PC stick).
                                        The first word of the Platform line decides, without case; no line or a word we do
                                        not know = psc)
  Category=PE                          (or the package type, see below)
  PeSource=<the .mod file name>
  PadMode=<pad= of the launcher's section in pe_compat.ini: psc, x360, psc-kernel or x360-kernel; psc-kernel without it>
  (an unknown pad value is a #WARN and psc-kernel; an unchanged package keeps its App, a changed one gets the current value)
  Dpad2Analog=<dpad2analog= of that section, 1 or 0; no line without it>
  Analog2Dpad=<analog2dpad= of that section, 1 or 0; no line without it>
  ```
- **A package type**: a control file that names one of the launcher's App categories - a metadata line of the
  Description (` Category: games`, as `pe_ports` writes it) or a field of its own (`Category: games`); games, emulators,
  tools, media or other, any case - files the App there: `Category=<type>` and the title gets the suffix ` (mod)`
  (`Title=OpenLara (mod)`). A package with no type (a user's own `.mod`), or a word that is no category, stays in the
  launcher's "PE apps" category (`Category=PE`) with its title as it is. The App is typed when it is made: a package
  already converted at the same version is not touched, so a newly typed package needs a new version.
- `readme.txt`: the control Description (its free-text lines, not its metadata), "Put the files this program needs
  (game data) in this folder.", and a one-line compatibility note - never the package's own README;
- `run.sh` (executable): `exec sh "${AB_RC_DIR:-/media/Autobleem/rc}/pe_run.sh" "$APP_DIR"`.

`launcher.cfg` is read as data (`name="value"` lines, CRLF, quotes of both kinds, comments) and never run. The
product's old name in any shown text is written as "PE".

The compatibility list (`[section]`, `block=1` or `skip=1`, `reason=`; a section is a `launcher_filename`, or the
folder's name) refuses a launcher with a `#WARN` and the reason; the rest of the package is still converted.

## Game data and engines (the packages spec, APPS-12)

- A launcher folder whose `launcher.cfg` says `launcher_package="1"` is **game data**: it needs no `launch.sh` and
  makes no App. It is unpacked into `Packages/pe-<launcher_filename>/` (`--packages`, else `$AB_PACKAGES_DIR`, else
  `Packages/` next to Apps/; made only when there is a package to put in it) with the game's files unchanged, minus
  `launcher.cfg` and `launch.sh`. The folder carries the descriptor `package.ini` (spec 2.2: `Title`, `Kind`,
  `Game<N>.Title`/`.File`, ...); proc_pe checks it (a Title, a content kind for every game, at least one game whose
  file is in the folder, no path outside it - `#WARN - ... not added (<why>)` and nothing added otherwise) and stamps
  `Version=<control Version>`, `Source=mod` and `PeSource=<the .mod>` (the mod's own values of these are replaced), plus
  `Image=<launcher_filename>.png` when the descriptor names none.
- The same rules as for an App: a folder without `PeSource` stays, the same or an older Version changes nothing, a newer
  Version replaces the package **whole** (a package keeps nothing of the old one), a marker in `Apps/.pe_state/`
  (`Packages=`) makes a second run a no-op.
- **An App an older proc_pe made from the same mod is removed** once the package is in place: `Apps/pe-<launcher_filename>`
  whose app.ini `PeSource=` names a `.mod` of the same package (`freedoomdata-0.13.0-1.mod` for `-2.mod`: the name up to
  the first `-` and a digit), together with that old `.mod`'s marker. Any other App stays.
- An engine's `launcher_uses="doom-iwad;heretic-iwad"` becomes `Uses=doom-iwad; heretic-iwad` and
  `launcher_package_dir="WAD;MODS"` becomes `PackageDir=WAD; MODS` in app.ini. A value that does not fit (a kind is
  `[a-z0-9]` with single `-`; a folder is relative, inside the App) is dropped with a `#WARN`; the App is still made.

## The rules it keeps

- **atomic**: everything is unpacked into `Apps/.pe_tmp/` (same filesystem) and a folder is renamed into `Apps/`
  only when complete. A killed run is cleared at the next start; a folder a killed replacement had moved aside is
  put back.
- **idempotent**: a package already converted (its marker in `Apps/.pe_state/`, same size and Version, its Apps
  there with the same `PeSource` and `Version`) is not even unpacked.
- **the `.mod` moves to `Mods/done/`** after a successful conversion (`--start --mods` only; a single `--mod` file
  stays where it is), so the files are not on the stick twice in plain sight - and it is not deleted, the user can take
  the original back. Success means the package was read to the end and its marker in `Apps/.pe_state/` is written
  (a launcher the list refuses is a `#WARN`, not a failure). A package that fails stays in `Mods/` and is tried again
  at every scan. `Mods/done/` is never scanned. A `.mod` dropped into `Mods/` again - the same name or a newer
  version - is converted again and replaces the copy in `done/`; a newer version under another file name also
  removes the old version's `.mod` from `done/`. The move never removes an App: this program never removes an App
  for a missing `.mod` (the marker, not the file, says what is converted). A move that fails (`Mods/done` is a file,
  the stick is read-only) is a `#WARN`; the App is complete and the `.mod` stays in `Mods/`.
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
pe --version                                  #PE app packages V<Version> - Turns PE app packages (.mod) into Apps
pe --ismine --mod <file>                      exit 0 = a .mod that is an ar archive, 1 = not mine
pe --start --mods <Mods dir> [--apps <dir>] [--packages <dir>] [--compat <file>]
pe --start --mod <file> [--apps <dir>] [--packages <dir>] [--compat <file>]
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
