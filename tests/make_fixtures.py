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


def control_text(package, version, description, extra_type="USB_MOD"):
    return (
        "Package: %s\nVersion: %s\nArchitecture: armhf\nMaintainer: ModMyClassic <contact@modmyclassic.com>\n"
        "Installed-Size: 10\nDescription: %s\n Type: %s\n The Project Eris package for %s.\n .\n Second paragraph.\n"
        " Author: Someone\n Platform: SONYPSC armhf\n Git Commit: abc123\n Built: 2020-07-16\n"
    ) % (package, version, description, extra_type, package)


def make_mod(path, package, version, launchers, description=None, control_xz=False, extra=(), control_type="USB_MOD",
             data_members=None):
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
    control = tar_bytes([("./control", control_text(package, version, description or package.title(), control_type).encode(), 0o644, "f")])
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

    # not a PE package: Type is something else; and a file that is no archive
    make_mod("othertype_1.0.mod", "other", "1.0", normal, "Other type", control_type="OTHER")
    with open(os.path.join(OUT, "garbage.mod"), "wb") as f:
        f.write(b"this is not a package\n")


if __name__ == "__main__":
    main()
