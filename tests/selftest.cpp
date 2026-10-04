//
// pe_selftest: copies the fixture packages in tests/data, runs the real pe program on them and checks what the
// protocol, the contract and the processor rules promise - the output lines, the exit codes, the Apps folders,
// and that a second run does nothing. Run by ctest from the build directory (a scratch tree is made there).
//
// tests/data is made by tests/make_fixtures.py (and dpkg_1.0.mod by make_dpkg_fixture.sh):
//   normal_1.0.mod     normalapp: a png, an executable, a subfolder, a symlink and a hardlink inside the folder; a
//                      second launcher folder's neighbours (an AutoBleem 1.x App, another opt folder) not unpacked
//   normal_1.1.mod     the same App, newer: another launch.sh, news.txt, no data/info.txt
//   normal_0.9.mod     the same App, older
//   two_1.0.mod        alpha and beta_launch (xz control archive)
//   blocked_1.0.mod    backupinternal_launch (blocked), goodone, and nolauncher (no cfg)
//   hybrid_1.0.mod     openbor (skipped by the list)
//   traversal / symlink / absolute _1.0.mod    names and links that reach outside the folder
//   crlf_1.0.mod       a launcher.cfg with CRLF, a comment, single quotes, a bare value
//   othertype_1.0.mod  Type: OTHER      garbage.mod   no archive at all
//   dpkg_1.0.mod       dpkgapp, made by the real dpkg-deb (xz control archive, root-owned entries)
//
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define MKDIR(p) _mkdir(p)
#define POPEN _popen
#define PCLOSE _pclose
#else
#include <sys/stat.h>
#include <sys/wait.h>
#define MKDIR(p) mkdir(p, 0777)
#define POPEN popen
#define PCLOSE pclose
#endif

using namespace std;

namespace {

int failures = 0;

void check(bool ok, const string &what) {
    if (!ok) {
        ++failures;
        cerr << "FAILED: " << what << endl;
    }
}

FILE *openFile(const string &path, const char *mode) {
#ifdef _WIN32
    auto wide = [](const string &s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        wstring w(n > 0 ? n - 1 : 0, L'\0');
        if (n > 1)
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
        return w;
    };
    return _wfopen(wide(path).c_str(), wide(mode).c_str());
#else
    return fopen(path.c_str(), mode);
#endif
}

bool exists(const string &path) {
    FILE *f = openFile(path, "rb");
    if (f)
        fclose(f);
    if (f)
        return true;
#ifndef _WIN32
    struct stat st;
    return stat(path.c_str(), &st) == 0; // a folder
#else
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
#endif
}

void writeFile(const string &path, const string &text) {
    ofstream(path, ios::binary) << text;
}

// a whole file; "<missing>" when it cannot be read
string readFile(const string &path) {
    FILE *f = openFile(path, "rb");
    if (!f)
        return "<missing>";
    string text;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        text.append(buf, n);
    fclose(f);
    return text;
}

bool executable(const string &path) {
#ifdef _WIN32
    return exists(path);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IXUSR) != 0;
#endif
}

void copyData(const string &name, const string &toDir) {
    writeFile(toDir + "/" + name, readFile(string(TEST_DATA) + "/" + name));
}

void makeDir(const string &path) {
    MKDIR(path.c_str());
}

void removeTree(const string &path) {
#ifdef _WIN32
    system(("if exist \"" + path + "\" rmdir /s /q \"" + path + "\"").c_str());
#else
    system(("rm -rf '" + path + "'").c_str());
#endif
}

// the bytes the fixtures hold (make_fixtures.py binary())
string binData(size_t n, int seed) {
    string s(n, '\0');
    for (size_t i = 0; i < n; ++i)
        s[i] = static_cast<char>((i * i / 3 + seed) & 0xFF);
    return s;
}

const string Launch = "#!/bin/sh\nsource /var/volatile/project_eris.cfg\ncd /var/volatile/launchtmp\n./game\n";

bool contains(const string &text, const string &part) {
    return text.find(part) != string::npos;
}

struct Run {
    int code;
    vector<string> lines;
    bool has(const string &line) const {
        for (const string &l : lines) {
            if (l == line)
                return true;
        }
        return false;
    }
    bool starts(const string &prefix) const {
        for (const string &l : lines) {
            if (l.compare(0, prefix.size(), prefix) == 0)
                return true;
        }
        return false;
    }
    bool mentions(const string &part) const {
        for (const string &l : lines) {
            if (contains(l, part))
                return true;
        }
        return false;
    }
};

Run run(const string &args) {
    string cmd = string("\"") + PE_EXE + "\" " + args;
#ifdef _WIN32
    cmd = "\"" + cmd + "\""; // cmd.exe strips one pair of quotes around the whole line
#endif
    Run r{-1, {}};
    FILE *p = POPEN(cmd.c_str(), "r");
    if (!p)
        return r;
    char buf[1024];
    while (fgets(buf, sizeof(buf), p)) {
        string line = buf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        r.lines.push_back(line);
    }
    int status = PCLOSE(p);
#ifdef _WIN32
    r.code = status;
#else
    r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    return r;
}

const string expectedRunSh = "#!/bin/sh\n"
                             "# PE App launcher - generated, do not edit\n"
                             "APP_DIR=\"$(cd \"$(dirname \"$0\")\" && pwd)\"\n"
                             "exec sh \"${AB_RC_DIR:-/media/Autobleem/rc}/pe_run.sh\" \"$APP_DIR\"\n";

} // namespace

int main() {
    const string root = "selftest_tree";
    removeTree(root);
    makeDir(root);
    const string mods = root + "/Mods";
    const string apps = root + "/Apps";
    makeDir(mods);
    for (const char *name : {"normal_1.0.mod", "normal_1.1.mod", "normal_0.9.mod", "garbage.mod"})
        copyData(name, mods);
    writeFile(mods + "/notes.txt", "not a package");

    Run v = run("--version");
    check(v.code == 0 && v.has("#PE app packages V1.0.0 - Turns PE app packages (.mod) into Apps"), "--version");
    check(run("--ismine --mod \"" + mods + "/normal_1.0.mod\"").code == 0, "a package: mine");
    check(run("--ismine --mod \"" + mods + "/garbage.mod\"").code == 1, "a .mod that is no archive: not mine");
    check(run("--ismine --mod \"" + mods + "/notes.txt\"").code == 1, "another file: not mine");

    // ---- a normal package
    const string app = apps + "/pe-normalapp";
    Run n = run("--start --mod \"" + mods + "/normal_1.0.mod\" --apps \"" + apps + "\"");
    check(n.lines.size() > 0 && n.lines[0] == "#Starting - PE app packages V1.0.0", "the first line is #Starting");
    check(n.code == 0 && n.has("#Converting normal_1.0.mod") && n.has("#Adding Normal App") && n.has("#DONE") &&
              n.has("100"),
          "a package converts: stage, app, percent, #DONE");
    check(readFile(app + "/launcher.cfg") ==
              "launcher_filename=\"normalapp\"\nlauncher_title=\"Normal App\"\nlauncher_year=\"2020\"\n"
              "launcher_publisher=\"An Author\"\n",
          "launcher.cfg is the package's, byte for byte");
    check(readFile(app + "/launch.sh") == Launch, "launch.sh is the package's");
    check(readFile(app + "/game") == binData(5000, 2) && executable(app + "/game"), "a binary, with its mode");
    check(readFile(app + "/data/info.txt") == "hello\n" && exists(app + "/normalapp.png"), "a subfolder, the png");
    check(readFile(app + "/game-link") == binData(5000, 2) && readFile(app + "/game-hard") == binData(5000, 2),
          "a symlink and a hardlink inside the folder become copies");
    check(readFile(app + "/app.ini") ==
              "Title=Normal App\nAuthor=An Author\nVersion=1.0\nImage=normalapp.png\nReadme=readme.txt\n"
              "Startup=run.sh\nExec.psc=run.sh\nCategory=PE\nPeSource=normal_1.0.mod\n",
          "app.ini as the contract has it");
    check(readFile(app + "/run.sh") == expectedRunSh && executable(app + "/run.sh"), "run.sh, executable");
    string readme = readFile(app + "/readme.txt");
    check(readme.compare(0, 13, "A normal app\n") == 0 && contains(readme, "Second paragraph.") &&
              contains(readme, "Put the files this program needs (game data) in this folder.") &&
              contains(readme, "Compatibility mode") && !contains(readme, "Type:") && !contains(readme, "Git Commit") &&
              !contains(readme, "Project Eris") && contains(readme, "The PE package for normalapp."),
          "readme.txt: the description, the game-data line, the compatibility note; no metadata, no old name");
    check(!exists(apps + "/other") && !exists(apps + "/pe-other") && !exists(apps + "/.pe_tmp"),
          "nothing else unpacked, no scratch left");

    // ---- a second run does nothing at all (a user's edit shows it is not unpacked again)
    writeFile(app + "/launch.sh", "edited by a user\n");
    Run again = run("--start --mod \"" + mods + "/normal_1.0.mod\" --apps \"" + apps + "\"");
    check(again.code == 0 && again.has("#DONE") && !again.starts("#Adding") && !again.has("100"),
          "an already converted package: no work");
    check(readFile(app + "/launch.sh") == "edited by a user\n", "and the folder is not touched");

    // ---- a newer version replaces the folder and keeps what the package did not ship
    makeDir(app + "/saves");
    writeFile(app + "/saves/slot1.sav", "my save");
    writeFile(app + "/game.dat", "my game data");
    writeFile(app + "/data/user.txt", "mine too");
    Run up = run("--start --mod \"" + mods + "/normal_1.1.mod\" --apps \"" + apps + "\"");
    check(up.code == 0 && up.has("#Adding Normal App") && up.has("#DONE"), "a newer version converts");
    check(readFile(app + "/launch.sh") == Launch + "echo v2\n" && readFile(app + "/news.txt") == "new in 1.1\n" &&
              readFile(app + "/game") == binData(5000, 3),
          "the new package's files replace the old ones");
    check(contains(readFile(app + "/app.ini"), "Version=1.1\n") &&
              contains(readFile(app + "/app.ini"), "PeSource=normal_1.1.mod\n"),
          "app.ini says the new version and file");
    check(readFile(app + "/saves/slot1.sav") == "my save" && readFile(app + "/game.dat") == "my game data" &&
              readFile(app + "/data/user.txt") == "mine too",
          "user files kept: at the root, in a new folder, inside a folder the package ships");
    check(!exists(apps + "/.pe_tmp") && !exists(apps + "/pe-normalapp.old"), "no scratch left after a replacement");

    // ---- an older one, and the same version again, leave it alone
    Run old = run("--start --mod \"" + mods + "/normal_0.9.mod\" --apps \"" + apps + "\"");
    check(old.code == 0 &&
              old.starts("#WARN - normal_0.9.mod: Normal App not added (a newer version, 1.1, is installed)") &&
              contains(readFile(app + "/app.ini"), "Version=1.1\n"),
          "an older package changes nothing and says why");
    Run oldAgain = run("--start --mod \"" + mods + "/normal_0.9.mod\" --apps \"" + apps + "\"");
    check(oldAgain.code == 0 && !oldAgain.starts("#WARN") && !oldAgain.starts("#Adding"), "and says it once");

    // ---- a folder that is not ours is never touched
    const string apps2 = root + "/Apps2";
    makeDir(apps2);
    makeDir(apps2 + "/pe-normalapp");
    writeFile(apps2 + "/pe-normalapp/app.ini", "Title=Mine\n");
    Run foreign = run("--start --mod \"" + mods + "/normal_1.0.mod\" --apps \"" + apps2 + "\"");
    check(foreign.code == 0 && foreign.mentions("exists and is not from a package") &&
              readFile(apps2 + "/pe-normalapp/app.ini") == "Title=Mine\n" && !exists(apps2 + "/pe-normalapp/launch.sh"),
          "an Apps folder without PeSource is left alone");

    // ---- a folder a killed replacement had moved aside is put back; stale scratch is cleared
    const string apps3 = root + "/Apps3";
    makeDir(apps3);
    makeDir(apps3 + "/.pe_tmp");
    makeDir(apps3 + "/.pe_tmp/pe-lost.old");
    writeFile(apps3 + "/.pe_tmp/pe-lost.old/save.dat", "kept");
    makeDir(apps3 + "/.pe_tmp/pkg");
    writeFile(apps3 + "/.pe_tmp/pkg/half.bin", "half");
    Run rec = run("--start --mod \"" + mods + "/normal_1.0.mod\" --apps \"" + apps3 + "\"");
    check(rec.code == 0 && readFile(apps3 + "/pe-lost/save.dat") == "kept" && !exists(apps3 + "/.pe_tmp"),
          "a folder moved aside by a killed run is put back; the scratch is cleared");

    // ---- a Mods folder: two launchers, a blocked one, a hybrid, CRLF, and everything unsafe
    const string mods2 = root + "/Mods2";
    makeDir(mods2);
    const char *all[] = {"two_1.0.mod",      "blocked_1.0.mod",   "hybrid_1.0.mod",
                         "crlf_1.0.mod",     "traversal_1.0.mod", "symlink_1.0.mod",
                         "absolute_1.0.mod", "othertype_1.0.mod", "garbage.mod"};
    for (const char *name : all)
        copyData(name, mods2);
    writeFile(mods2 + "/notes.txt", "not a package");
    const string apps4 = root + "/Apps4";
    Run m = run("--start --mods \"" + mods2 + "\" --apps \"" + apps4 + "\"");
    check(m.code == 1 && m.starts("#ERROR - 5 of 9 packages could not be converted"), "the unsafe ones fail the run");
    check(m.has("9/9") || m.has("8/9"), "the packages are counted");
    check(readFile(apps4 + "/pe-alpha/app.ini") ==
              "Title=Alpha\nAuthor=ModMyClassic\nVersion=2.0-1\nImage=alpha.png\nReadme=readme.txt\nStartup=run.sh\n"
              "Exec.psc=run.sh\nCategory=PE\nPeSource=two_1.0.mod\n",
          "two launchers: the first (the author is the maintainer, without the address)");
    check(readFile(apps4 + "/pe-beta/app.ini") ==
              "Title=Beta\nAuthor=Beta "
              "Author\nVersion=2.0-1\nReadme=readme.txt\nStartup=run.sh\nExec.psc=run.sh\nCategory=PE\n"
              "PeSource=two_1.0.mod\n",
          "two launchers: the second, named by its launcher_filename, no png no Image");
    check(m.mentions("blocked_1.0.mod: Backup not added (deletes the console's own games)") &&
              !exists(apps4 + "/pe-backupinternallaunch") && exists(apps4 + "/pe-goodone/launch.sh"),
          "a blocked launcher is refused with the reason, the good one in the same package is added");
    check(m.mentions("blocked_1.0.mod: nolauncher is not a launcher folder"), "a folder with no cfg is no launcher");
    check(m.mentions("hybrid_1.0.mod: OpenBOR not added (AutoBleem has its own App)") &&
              !exists(apps4 + "/pe-openbor") && !exists(apps4 + "/openbor"),
          "a hybrid is skipped with the list's reason, its embedded App not unpacked");
    check(readFile(apps4 + "/pe-crlfapp/app.ini") ==
              "Title=Crlf "
              "App\nAuthor=Bare\nVersion=1.0\nReadme=readme.txt\nStartup=run.sh\nExec.psc=run.sh\nCategory=PE\n"
              "PeSource=crlf_1.0.mod\n",
          "a CRLF launcher.cfg with quotes of both kinds, a comment and a bare value");
    check(contains(readFile(apps4 + "/pe-crlfapp/launcher.cfg"), "\r\n"), "and the file itself keeps its CRLF");
    check(m.mentions("traversal_1.0.mod: unsafe name in the package") && !exists(apps4 + "/pe-evilapp") &&
              !exists(root + "/escaped.txt") && !exists("escaped.txt") && !exists(apps4 + "/escaped.txt"),
          "a name with .. refuses the package, nothing is written outside");
    check(m.mentions("symlink_1.0.mod: a link in the package points outside its folder"), "a link out is refused");
    check(m.mentions("absolute_1.0.mod: unsafe name in the package") && !exists("/tmp/escaped_absolute.txt"),
          "an absolute name is refused");
    check(m.mentions("othertype_1.0.mod: not a PE app package (Type OTHER)"), "another package type is refused");
    check(m.mentions("garbage.mod: not a PE app package"), "a file that is no archive is refused");
    check(!exists(apps4 + "/.pe_tmp"), "no scratch left after failures");

    // the same run again: the good ones are known, only the failures are tried (and fail) again
    Run m2 = run("--start --mods \"" + mods2 + "\" --apps \"" + apps4 + "\"");
    check(m2.code == 1 && m2.starts("#ERROR - 5 of 9") && !m2.mentions("Backup not added") && !m2.mentions("#Adding"),
          "a second run: converted packages are not unpacked or warned about again");

    // ---- a list of its own
    writeFile(root + "/compat.ini", "[alpha]\nskip=1\nreason=test reason\n");
    const string apps5 = root + "/Apps5";
    Run c = run("--start --mod \"" + mods2 + "/two_1.0.mod\" --apps \"" + apps5 + "\" --compat \"" + root +
                "/compat.ini\"");
    check(c.code == 0 && c.mentions("Alpha not added (test reason)") && !exists(apps5 + "/pe-alpha") &&
              exists(apps5 + "/pe-beta/app.ini"),
          "--compat: the file's list is the list");

    // ---- a section with neither block nor skip (the launcher's `remap=` for a mod) and unknown keys: convert normally
    writeFile(root + "/compat2.ini", "[beta]\nremap=some_remap.so\nfuture_key=1\n[alpha]\n; a comment\nnote=x\n");
    const string apps9 = root + "/Apps9";
    Run rm = run("--start --mod \"" + mods2 + "/two_1.0.mod\" --apps \"" + apps9 + "\" --compat \"" + root +
                 "/compat2.ini\"");
    check(rm.code == 0 && !rm.starts("#WARN") && !rm.starts("#ERROR") && exists(apps9 + "/pe-beta/app.ini") &&
              exists(apps9 + "/pe-alpha/app.ini") && readFile(apps9 + "/pe-beta/launch.sh") == Launch,
          "a section with only remap= (or unknown keys) converts normally, no warning");

    // ---- a package the real dpkg-deb made
    const string apps7 = root + "/Apps7";
    copyData("dpkg_1.0.mod", mods2);
    Run d = run("--start --mod \"" + mods2 + "/dpkg_1.0.mod\" --apps \"" + apps7 + "\"");
    check(d.code == 0 && d.has("#Adding Dpkg App") &&
              readFile(apps7 + "/pe-dpkgapp/app.ini") ==
                  "Title=Dpkg App\nAuthor=Real "
                  "Maker\nVersion=1.0\nReadme=readme.txt\nStartup=run.sh\nExec.psc=run.sh\nCategory=PE\n"
                  "PeSource=dpkg_1.0.mod\n" &&
              readFile(apps7 + "/pe-dpkgapp/dpkgapp") == "program bytes\n" &&
              executable(apps7 + "/pe-dpkgapp/launch.sh") &&
              contains(readFile(apps7 + "/pe-dpkgapp/readme.txt"), "Built by the real tool."),
          "a package made by dpkg-deb");

    // ---- a description with " Author:" glued to its last line (the packers' Makefile does that)
    const string apps8 = root + "/Apps8";
    copyData("glued_1.0.mod", mods2);
    Run gl = run("--start --mod \"" + mods2 + "/glued_1.0.mod\" --apps \"" + apps8 + "\"");
    string gluedReadme = readFile(apps8 + "/pe-gluedapp/readme.txt");
    check(gl.code == 0 && gl.has("#Adding Glued App") && contains(gluedReadme, "Second paragraph.\n") &&
              !contains(gluedReadme, "Author") && contains(readFile(apps8 + "/pe-gluedapp/app.ini"), "Version=1.0\n"),
          "a glued ' Author:' is cut from the readme and the rest of the control file still parses");

    // ---- nothing to do
    makeDir(root + "/Empty");
    Run none = run("--start --mods \"" + root + "/Empty\" --apps \"" + root + "/Apps6\"");
    check(none.code == 0 && none.lines.size() == 2 && none.lines[1] == "#DONE",
          "no packages: #Starting and #DONE only");

    if (failures == 0)
        cout << "pe_selftest: all checks passed" << endl;
    return failures == 0 ? 0 : 1;
}
