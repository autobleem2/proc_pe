# proc_pe - developer context

An AutoBleem **scanner processor** (the launcher's `docs/scanner-processors-plan.md`): a plain console program in
`System/Processors/pe/` that turns PE app packages (`.mod`, Debian `ar` archives) into Apps. The README has the
output contract (app.ini, readme.txt, run.sh), the rules and the protocol; `src/main.cpp` is one file with the
same order as proc_unzip's: the protocol in the header comment, a small portable filesystem layer, then the work.
It is the sibling of proc_unzip - same layout, build and CI - and was cloned from it.

- **No AutoBleem SDK**: the C++ standard library and vendored decoders.
  - `third_party/libarchive` - libarchive **3.8.9** (BSD-2-Clause), trimmed to the read core, the **tar** reader and
    the **xz** filter; `third_party/xz` - liblzma **5.8.4** (0BSD), decoders only (the .xz container, LZMA2, CRC32/
    CRC64/SHA-256 checks). `third_party/archive.cmake` lists the sources; configuration is by hand
    (`libarchive/ab_config.h`) - no configure checks, gcc-6 for the console included. No iconv, zlib, bzip2.
    The `ar` container is parsed by `readAr()` (60-byte headers), not by libarchive: the data member is read as a
    byte range of the file, streamed (a package is never in memory; the console has 512 MB).
  - `third_party/miniz` (MIT): only `tinfl` for a gzip'd `control.tar.gz` (old packages), through `gunzip()`.
  - Names: `libarchive/ab_names.c` sets libarchive's charset to UTF-8 (a private field - re-check on an update);
    `archive_entry_pathname()` is UTF-8 on Linux, the `_utf8` variants on Windows. Do not mix them up.
- **One pass over the data**: launcher folders are unpacked into `Apps/.pe_tmp/pkg/<dir>/` as the tar goes by (the
  cfg may come after the files), then each folder is checked (cfg, compat list, installed version), gets its three
  generated files and is renamed into `Apps/` (`installFolder()`; a replacement moves the old folder to
  `.pe_tmp/<name>.old`, merges what the new one lacks into the new one - `mergeOld()` - and renames the new in;
  `recover()` undoes a killed run). `removeTree()` refuses any path outside `.pe_tmp`.
- **No-op check without unpacking**: `Apps/.pe_state/<mod>.ini` (Version, Size, the Apps made) plus the control
  file's Version (a few KB of the ar) - `alreadyConverted()`.
- **`Mods/done/`** (owner, 2026-10-05): `start()` calls `retire()` after each package whose `convert()` is ok and only
  in `--mods` mode (a `--mod` file stays - the tests reuse their fixtures that way). It replaces `done/<name>` and also
  deletes the `done/` copy of the package whose App a newer version replaced (`Result::replacedSources`). `listDir`
  skips folders, so `done/` is never scanned. Nothing in this program removes an App for a missing `.mod`; the
  launcher and the Store treat "installed" as the .mod in Mods/ or Mods/done/ or the marker.
- The product's old name must not reach a user: `shown()` cleans every value that goes into app.ini/readme/#WARN
  lines. Code comments may cite the technical path `media/project_eris/...`.
- **Tests**: `tests/selftest.cpp` (ctest; natively and again for i386 in CI) over `tests/data/*.mod`, made by
  `tests/make_fixtures.py` (run it again to change them; deterministic) and `tests/make_dpkg_fixture.sh` (the real
  `dpkg-deb`, on the laptop).
- **Build**: `ci/build.sh native|psc|rpi|rpi64|pcusb|win|package|all` in `ghcr.io/autobleem2/autobleem-build`;
  `./make_win.sh` on Windows (MSYS2 UCRT64). The package is `dist/pe-<version>.zip` (`System/Processors/pe/`).
  The version is `package/processor.ini`'s `Version=`.
- **CI**: `.github/workflows/build.yml`, gated by `AB_CI_ENABLED` like every autobleem2 repository.
- The contract with the launcher's runtime (`rc/pe_run.sh`, `rc/pe_compat.ini`) is in the README; the two must
  change together.
