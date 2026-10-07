#!/usr/bin/env python3
"""Builds the fixture packages in tests/data/ (committed; run again only to change them).

A .mod is a Debian archive: an ar file with debian-binary, control.tar.{gz,xz} and data.tar.xz. These are made
with the Python standard library so that unsafe ones (a ".." name, a link out) can be made too; a real one made
by dpkg-deb is tests/data/dpkg_1.0.mod (see make_dpkg_fixture.sh). Everything is deterministic (times 0).

    python tests/make_fixtures.py
"""
import gzip
import io
import lzma
import os
import tarfile

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
PREFIX = "./media/project_eris/etc/project_eris/SUP/launchers/"


def tar_bytes(members):
    """members: (name, data|None, mode, kind) with kind 'f' file, 'd' dir, 's' symlink (data = target), 'h' hardlink."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.GNU_FORMAT) as t:
        for name, data, mode, kind in members:
            info = tarfile.TarInfo(name)
            info.mtime = 0
            info.uname = info.gname = "root"
            info.mode = mode
            if kind == "d":
                info.type = tarfile.DIRTYPE
                t.addfile(info)
            elif kind == "s":
                info.type = tarfile.SYMTYPE
                info.linkname = data
                t.addfile(info)
            elif kind == "h":
                info.type = tarfile.LNKTYPE
                info.linkname = data
                t.addfile(info)
            else:
                info.size = len(data)
                t.addfile(info, io.BytesIO(data))
    return buf.getvalue()


def ar_bytes(members):
    out = b"!<arch>\n"
    for name, data in members:
        out += (name + "/").ljust(16).encode() + b"0".ljust(12) + b"0".ljust(6) + b"0".ljust(6) + b"100644".ljust(8)
        out += str(len(data)).encode().ljust(10) + b"`\n" + data
        if len(data) % 2:
            out += b"\n"
    return out


def control_text(package, version, description, extra_type="USB_MOD", glue_author=False, category=None,
                 platform="SONYPSC armhf"):
    """platform: the Platform line's value ("RPI armhf" is a Raspberry Pi package); None leaves the line out"""
    text = (
        "Package: %s\nVersion: %s\nArchitecture: armhf\nMaintainer: ModMyClassic <contact@modmyclassic.com>\n"
        "Installed-Size: 10\nDescription: %s\n Type: %s\n The Project Eris package for %s.\n .\n Second paragraph.\n"
        " Author: Someone\n%s Git Commit: abc123\n Built: 2020-07-16\n"
    ) % (package, version, description, extra_type, package, " Platform: %s\n" % platform if platform else "")
    if glue_author:
        text = text.replace(" Second paragraph.\n Author: Someone\n", " Second paragraph. Author: Someone\n")
    if category:
        # a package type, the way pe_ports' mkmod.py writes it: a metadata line of the Description ("Category: x"),
        # or - with a leading "!" - a field of its own
        if category.startswith("!"):
            text = text.replace("Installed-Size: 10\n", "Installed-Size: 10\nCategory: %s\n" % category[1:])
        else:
            text = text.replace(" Type: %s\n" % extra_type, " Type: %s\n Category: %s\n" % (extra_type, category))
    return text


def make_mod(path, package, version, launchers, description=None, control_xz=False, extra=(), control_type="USB_MOD",
             data_members=None, glue_author=False, category=None, platform="SONYPSC armhf"):
    """launchers: {dir: {relpath: bytes | (bytes, mode)}}; extra: more data members."""
    members = [("./", None, 0o755, "d")]
    for d, files in launchers.items():
        members.append((PREFIX + d + "/", None, 0o755, "d"))
        for rel, content in files.items():
            mode = 0o644
            if isinstance(content, tuple):
                content, mode = content
            members.append((PREFIX + d + "/" + rel, content, mode, "f"))
    members.extend(extra)
    control = tar_bytes([("./control", control_text(package, version, description or package.title(), control_type, glue_author, category, platform).encode(), 0o644, "f")])
    if control_xz:
        control_member = ("control.tar.xz", lzma.compress(control, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64))
    else:
        control_member = ("control.tar.gz", gzip.compress(control, mtime=0))
    data = lzma.compress(tar_bytes(data_members if data_members is not None else members), format=lzma.FORMAT_XZ,
                         check=lzma.CHECK_CRC64)
    with open(os.path.join(OUT, path), "wb") as f:
        f.write(ar_bytes([("debian-binary", b"2.0\n"), control_member, ("data.tar.xz", data)]))


def cfg(filename, title, publisher=None, eol="\n"):
    lines = ['launcher_filename="%s"' % filename, 'launcher_title="%s"' % title, 'launcher_year="2020"']
    if publisher:
        lines.append('launcher_publisher="%s"' % publisher)
    return (eol.join(lines) + eol).encode()


LAUNCH = b"#!/bin/sh\nsource /var/volatile/project_eris.cfg\ncd /var/volatile/launchtmp\n./game\n"


def binary(n, seed=7):
    return bytes((i * i // 3 + seed) & 0xFF for i in range(n))


def main():
    os.makedirs(OUT, exist_ok=True)

    # a normal package: gzip'd control, a png, a binary, a subfolder, a link inside the folder, and things the
    # processor must not unpack (another folder of the data, an AutoBleem 1.x App)
    normal = {
        "normalapp": {
            "launcher.cfg": cfg("normalapp", "Normal App", "An Author"),
            "launch.sh": (LAUNCH, 0o755),
            "normalapp.png": binary(300, 1),
            "game": (binary(5000, 2), 0o755),
            "data/info.txt": b"hello\n",
        }
    }
    extra = [
        (PREFIX + "normalapp/game-link", "game", 0o777, "s"),
        (PREFIX + "normalapp/game-hard", PREFIX[2:] + "normalapp/game", 0o644, "h"),
        ("./media/Autobleem/Apps/other/app.ini", b"Title=Other\n", 0o644, "f"),
        ("./media/project_eris/opt/other/x.json", b"{}", 0o644, "f"),
    ]
    make_mod("normal_1.0.mod", "normalapp", "1.0", normal, "A normal app", extra=extra)

    # the same package, a newer version: another launch.sh, a new file, no data/info.txt, and a root file that a
    # user's file will clash with
    normal2 = {
        "normalapp": {
            "launcher.cfg": cfg("normalapp", "Normal App", "An Author"),
            "launch.sh": (LAUNCH + b"echo v2\n", 0o755),
            "normalapp.png": binary(300, 1),
            "game": (binary(5000, 3), 0o755),
            "news.txt": b"new in 1.1\n",
        }
    }
    make_mod("normal_1.1.mod", "normalapp", "1.1", normal2, "A normal app")
    make_mod("normal_0.9.mod", "normalapp", "0.9", normal, "A normal app")

    # the machine a package is built for (APPS-13): the control file's Platform line picks app.ini's Exec.<key>=: the
    # Raspberry Pi 32-bit ("RPI armhf") Exec.rpi, the console's, none and an unknown word Exec.psc
    for name, platform in (("rpi", "RPI armhf"), ("noplat", None), ("oddplat", "SOMETHING arm64"), ("rpicase", "rpi")):
        launcher = {name + "app": {"launcher.cfg": cfg(name + "app", name.title() + " App"), "launch.sh": LAUNCH}}
        make_mod("platform-%s_1.0.mod" % name, name + "app", "1.0", launcher, "Built for " + name, platform=platform)

    # package types: the control file's "Category" (a Description line, or a field of its own) files the App in
    # that category as "<title> (mod)"; a word that is no category ("pe") is an untyped package
    typed = {"typedapp": {"launcher.cfg": cfg("typedapp", "Typed App", "An Author"), "launch.sh": LAUNCH}}
    make_mod("typed_1.0.mod", "typedapp", "1.0", typed, "A typed app", category="Games")
    typed_top = {"toolapp": {"launcher.cfg": cfg("toolapp", "Tool App"), "launch.sh": LAUNCH}}
    make_mod("typedtop_1.0.mod", "toolapp", "1.0", typed_top, "A tool", category="!tools")
    untyped_word = {"oddapp": {"launcher.cfg": cfg("oddapp", "Odd App"), "launch.sh": LAUNCH}}
    make_mod("oddtype_1.0.mod", "oddapp", "1.0", untyped_word, "An odd one", category="pe")

    # game data (the packages spec, APPS-12): launcher_package="1" and a package.ini in the launcher folder. The
    # first version of "gamedata" is the old kind (a launch.sh and no mark: older proc_pe made an App of it), the
    # second one is the data package; an engine names the kinds it runs
    wad_a, wad_b = binary(3000, 11), binary(2000, 12)
    old_data = {"gamedata": {"launcher.cfg": cfg("gamedata", "Game Data", "A Maker"), "launch.sh": LAUNCH,
                             "a.wad": wad_a, "b.wad": wad_b}}
    make_mod("gamedata-1.0-1.mod", "gamedata", "1.0-1", old_data, "Game data, the old way")
    pkg_ini = (b"[package]\nTitle=Game Data\nKind=doom-iwad\nLicence=BSD-3-Clause\nAuthor=A Maker\n"
               b"Source=store\nPeSource=bogus.mod\nVersion=0.0\n"
               b"Game1.Id=first\nGame1.Title=First Game\nGame1.File=a.wad\n"
               b"Game2.Id=second\nGame2.Title=Second Game\nGame2.File=Sub/B.WAD\n")
    data = {"gamedata": {"launcher.cfg": cfg("gamedata", "Game Data", "A Maker") + b'launcher_package="1"\n',
                         "package.ini": pkg_ini, "a.wad": wad_a, "Sub/b.wad": wad_b, "gamedata.png": binary(100, 5),
                         "licences/COPYING.txt": b"licence\n", "SOURCE.txt": b"source\n"}}
    make_mod("gamedata-1.0-2.mod", "gamedata", "1.0-2", data, "Game data")
    data3 = {"gamedata": dict(data["gamedata"], **{"a.wad": binary(3000, 13)})}
    make_mod("gamedata-1.0-3.mod", "gamedata", "1.0-3", data3, "Game data, newer")
    # a data mod with a symbolic link inside the folder (the stick is FAT: it becomes a copy)
    linked = {"linkdata": {"launcher.cfg": cfg("linkdata", "Link Data") + b'launcher_package="yes"\n',
                           "package.ini": b"Title=Link Data\nKind=dos-game\nGame1.Title=Linked\nGame1.File=GAME.EXE\n",
                           "real.exe": binary(500, 21)}}
    make_mod("linkdata-1.0-1.mod", "linkdata", "1.0-1", linked, "Link data",
             extra=[(PREFIX + "linkdata/GAME.EXE", "real.exe", 0o777, "s")])
    # data mods that are refused: no package.ini, a game file that is not there, a kind that is not a kind
    nodesc = {"nodesc": {"launcher.cfg": cfg("nodesc", "No Descriptor") + b'launcher_package="1"\n', "a.wad": wad_a}}
    make_mod("nodesc-1.0-1.mod", "nodesc", "1.0-1", nodesc, "No package.ini")
    nofile = {"nofile": {"launcher.cfg": cfg("nofile", "No File") + b'launcher_package="1"\n',
                         "package.ini": b"Title=No File\nKind=doom-iwad\nGame1.Title=Gone\nGame1.File=gone.wad\n"}}
    make_mod("nofile-1.0-1.mod", "nofile", "1.0-1", nofile, "A game file that is not there")
    badkind = {"badkind": {"launcher.cfg": cfg("badkind", "Bad Kind") + b'launcher_package="1"\n',
                           "package.ini": b"Title=Bad Kind\nKind=Doom IWAD\nGame1.Title=G\nGame1.File=a.wad\n",
                           "a.wad": wad_a}}
    make_mod("badkind-1.0-1.mod", "badkind", "1.0-1", badkind, "A kind with a blank")
    escape = {"escape": {"launcher.cfg": cfg("escape", "Escape") + b'launcher_package="1"\n',
                         "package.ini": b"Title=Escape\nKind=doom-iwad\nGame1.Title=G\nGame1.File=../a.wad\n",
                         "a.wad": wad_a}}
    make_mod("escape-1.0-1.mod", "escape", "1.0-1", escape, "A game file outside the package")
    # an engine that runs game packages: Uses= and PackageDir= from launcher.cfg; a bad value is dropped with a #WARN
    engine = {"engine": {"launcher.cfg": cfg("engine", "Engine") + b'launcher_uses="doom-iwad; Heretic-IWAD"\n'
                         b'launcher_package_dir="WAD;MODS"\n', "launch.sh": LAUNCH}}
    make_mod("engine-1.0-1.mod", "engine", "1.0-1", engine, "An engine")
    badengine = {"badengine": {"launcher.cfg": cfg("badengine", "Bad Engine") + b'launcher_uses="doom iwad"\n'
                               b'launcher_package_dir="../out"\n', "launch.sh": LAUNCH}}
    make_mod("badengine-1.0-1.mod", "badengine", "1.0-1", badengine, "An engine with bad values")

    # two launchers in one package, an xz control archive
    two = {
        "alpha": {"launcher.cfg": cfg("alpha", "Alpha"), "launch.sh": LAUNCH, "alpha.png": binary(100)},
        "beta_launch": {"launcher.cfg": cfg("beta", "Beta", "Beta Author"), "launch.sh": LAUNCH},
    }
    make_mod("two_1.0.mod", "twoapps", "2.0-1", two, "Two apps", control_xz=True)

    # a blocked launcher next to a good one, and a folder that is no launcher
    blocked = {
        "backupinternal_launch": {"launcher.cfg": cfg("backupinternallaunch", "Backup"), "launch.sh": b"rm -Rf /gaadata/1\n"},
        "goodone": {"launcher.cfg": cfg("goodone", "Good One"), "launch.sh": LAUNCH},
        "nolauncher": {"readme.txt": b"no cfg here\n"},
    }
    make_mod("blocked_1.0.mod", "blockedmix", "1.0", blocked, "A blocked one and a good one")

    # an AutoBleem 1.x hybrid: skipped, its embedded App is not unpacked
    hybrid = {"openbor": {"launcher.cfg": cfg("openbor", "OpenBOR"), "launch.sh": LAUNCH}}
    make_mod("hybrid_1.0.mod", "openbor", "1.0", hybrid, "Hybrid",
             extra=[("./media/Autobleem/Apps/openbor/app.ini", b"Title=OpenBOR\n", 0o644, "f")])

    # a name that climbs out of the folder
    evil = {"evilapp": {"launcher.cfg": cfg("evilapp", "Evil"), "launch.sh": LAUNCH}}
    make_mod("traversal_1.0.mod", "evil", "1.0", evil, "Traversal",
             extra=[(PREFIX + "evilapp/../../../../../../../../escaped.txt", b"escaped\n", 0o644, "f")])
    # a symbolic link out of the folder
    make_mod("symlink_1.0.mod", "evillink", "1.0", evil, "Symlink out",
             extra=[(PREFIX + "evilapp/passwd", "../../../../../../../../../../etc/passwd", 0o777, "s")])
    # an absolute name
    make_mod("absolute_1.0.mod", "evilabs", "1.0", evil, "Absolute",
             extra=[("/tmp/escaped_absolute.txt", b"escaped\n", 0o644, "f")])

    # a launcher.cfg written on Windows: CRLF, a comment, single quotes, a bare value, spaces
    crlf = {
        "crlfapp": {
            "launcher.cfg": b"# made on Windows\r\nlauncher_filename='crlfapp'\r\n  launcher_title = \"Crlf App\"  \r\n"
                            b"launcher_publisher=Bare\r\nlauncher_title_note=\"$(rm -rf /)\"\r\n",
            "launch.sh": LAUNCH,
        }
    }
    make_mod("crlf_1.0.mod", "crlfapp", "1.0", crlf, "Crlf")

    # a description with no trailing newline: the Makefile glues " Author:" on to its last line
    glued = {"gluedapp": {"launcher.cfg": cfg("gluedapp", "Glued App"), "launch.sh": LAUNCH}}
    make_mod("glued_1.0.mod", "gluedapp", "1.0", glued, "Glued", glue_author=True)

    # not a PE package: Type is something else; and a file that is no archive
    make_mod("othertype_1.0.mod", "other", "1.0", normal, "Other type", control_type="OTHER")
    with open(os.path.join(OUT, "garbage.mod"), "wb") as f:
        f.write(b"this is not a package\n")


if __name__ == "__main__":
    main()
