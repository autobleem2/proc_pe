//
// proc_pe - an AutoBleem scanner processor that turns PE app packages (.mod) into AutoBleem Apps.
//
// A package is a Debian archive (ar: debian-binary, control.tar.{gz,xz}, data.tar.xz) holding launcher folders:
// media/project_eris/etc/project_eris/SUP/launchers/<dir>/ with a launcher.cfg and a launch.sh. Each folder
// that the compatibility list allows becomes one App, Apps/pe-<launcher_filename>/: the folder byte for byte
// as the package has it, plus three generated files - app.ini, readme.txt and run.sh (the contract in the
// README). A package converted from a Mods folder (--mods) moves to Mods/done/ afterwards (replacing a copy
// there; never deleted; done/ is never scanned; a failed package stays in Mods and is tried again); a single
// --mod file stays where it is. The launcher's runtime (rc/pe_run.sh) runs the folder's own launch.sh. This
// program never executes anything from a package.
//
//   pe --version                                    "#PE app packages V1.0.0 - <what it does>"
//   pe --ismine --mod <file>                        exit 0 = mine (a .mod in the ar format), 1 = not mine
//   pe --start --mods <Mods dir> [--apps <dir>]     every *.mod in the folder
//   pe --start --mod <file> [--apps <dir>]          one package
//
// The Apps folder is --apps, else $AB_APPS_DIR, else Apps/ next to the Mods folder. The compatibility list is
// --compat, else $AB_PE_COMPAT, else rc/pe_compat.ini under $AB_ROOT (or $AB_ROOT/Autobleem), else the copy
// built in. What it prints (one line at a time, flushed): #Starting - <title>, #Converting <file>, n/m, 0..100,
// #Adding <title>, #WARN - <text>, and at the end #DONE (exit 0) or #ERROR - <text> (exit 1).
//
// The rules it keeps, which every processor must keep:
//   - atomic: a package is unpacked into Apps/.pe_tmp/ first (same filesystem) and a folder is renamed into
//     Apps/ only when complete. It can be killed at any moment and started again: it clears .pe_tmp first, and
//     puts a folder that a killed replacement had moved aside back.
//   - idempotent: a package already converted (its marker in Apps/.pe_state/ and its Apps still there with the
//     same PeSource and Version) is not even unpacked - #Starting and #DONE only.
//   - it never overwrites what it did not make: an Apps folder without PeSource in its app.ini is left alone;
//     a package with the same or an older Version than the installed one changes nothing; a newer Version
//     replaces the folder but keeps every file of the old folder that the new package does not ship (saves,
//     game data - merged in, at any depth).
//   - safe unpacking: only the launcher folders are unpacked; a name with "..", an absolute name or a
//     backslash anywhere in the package refuses the whole package; a link must point inside its own launcher
//     folder (and is stored as a copy - the stick is FAT); size limits on a file, a package and the entry count.
//
#include "miniz.h"

#include "ab_names.h"
#include "archive.h"
#include "archive_entry.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace std;

namespace {

const char *Version = "1.0.0";
const char *Description = "Turns PE app packages (.mod) into Apps";

// the launcher folders inside a package's data
const char *LauncherPrefix = "media/project_eris/etc/project_eris/SUP/launchers/";

// limits: a launcher folder is a program and its data, on a FAT stick (4 GB a file)
const uint64_t MaxFileSize = 2048ull * 1024 * 1024;
const uint64_t MaxPackageSize = 3072ull * 1024 * 1024;
const size_t MaxEntries = 50000;
const size_t MaxPathLength = 1000;
const size_t MaxNameLength = 255;
const size_t MaxMemberInMemory = 8 * 1024 * 1024; // the control archive, in memory

// the pad output of a launcher the list has no pad= for: the console's pad as a real device, which every mod reaches
// (its own SDL, a static one, or raw evdev); the shim's "psc" is a choice the user makes in the launcher
const string DefaultPadMode = "psc-kernel";

// the compatibility list, as the launcher ships it (rc/pe_compat.ini); used when no file is found
const char *BuiltInCompat =
    "[backupinternallaunch]\nblock=1\nreason=deletes the console's own games\n"
    "[editinternallaunch]\nblock=1\nreason=deletes the console's own games\n"
    "[restoreinternallaunch]\nblock=1\nreason=deletes the console's own games\n"
    "[bootmenu]\nskip=1\n[folder]\nskip=1\n[gamemanager]\nskip=1\n[pehome]\nskip=1\n[retroarch]\nskip=1\n"
    "[openbor]\nskip=1\nreason=AutoBleem has its own App\n"
    "[doom]\nskip=1\nreason=AutoBleem has its own App\n"
    "[amiberry]\nskip=1\nreason=AutoBleem has its own App\n";

//******************
// the protocol's output
//******************
// every line is flushed at once: the launcher shows progress as it comes, and a pipe is block-buffered
void say(const string &line) {
    fputs(line.c_str(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

//******************
// a little portable filesystem (UTF-8 paths on every system)
//******************
#ifdef _WIN32
wstring wide(const string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
string narrow(const wstring &w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}
#endif

struct Entry {
    string name;
    bool isDir;  // a real folder: a link to one is not
    bool isLink; // a symbolic link (a reparse point on Windows): never followed
};

vector<Entry> listDir(const string &dir) {
    vector<Entry> out;
#ifdef _WIN32
    WIN32_FIND_DATAW data;
    HANDLE h = FindFirstFileW(wide(dir + "/*").c_str(), &data);
    if (h == INVALID_HANDLE_VALUE)
        return out;
    do {
        string name = narrow(data.cFileName);
        if (name == "." || name == "..")
            continue;
        bool link = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        out.push_back({name, !link && (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, link});
    } while (FindNextFileW(h, &data));
    FindClose(h);
#else
    DIR *d = opendir(dir.c_str());
    if (!d)
        return out;
    while (dirent *e = readdir(d)) {
        string name = e->d_name;
        if (name == "." || name == "..")
            continue;
        struct stat st;
        bool known = lstat((dir + "/" + name).c_str(), &st) == 0;
        out.push_back({name, known && S_ISDIR(st.st_mode), known && S_ISLNK(st.st_mode)});
    }
    closedir(d);
#endif
    sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) { return a.name < b.name; });
    return out;
}

bool exists(const string &path) {
#ifdef _WIN32
    return GetFileAttributesW(wide(path).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return lstat(path.c_str(), &st) == 0;
#endif
}

bool isDir(const string &path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesW(wide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    struct stat st;
    return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool makeDirs(const string &path) {
    if (path.empty() || isDir(path))
        return true;
    size_t slash = path.find_last_of('/');
    if (slash != string::npos && slash > 0 && !makeDirs(path.substr(0, slash)))
        return false;
#ifdef _WIN32
    return CreateDirectoryW(wide(path).c_str(), nullptr) || isDir(path);
#else
    return mkdir(path.c_str(), 0777) == 0 || isDir(path);
#endif
}

bool removeFile(const string &path) {
#ifdef _WIN32
    return DeleteFileW(wide(path).c_str()) != 0;
#else
    return unlink(path.c_str()) == 0;
#endif
}

bool removeEmptyDir(const string &path) {
#ifdef _WIN32
    return RemoveDirectoryW(wide(path).c_str()) != 0;
#else
    return rmdir(path.c_str()) == 0;
#endif
}

bool renameFile(const string &from, const string &to) {
#ifdef _WIN32
    return MoveFileW(wide(from).c_str(), wide(to).c_str()) != 0;
#else
    return rename(from.c_str(), to.c_str()) == 0;
#endif
}

void setMode(const string &path, unsigned mode) {
#ifndef _WIN32
    chmod(path.c_str(), static_cast<mode_t>(mode & 0777));
#else
    (void)path;
    (void)mode;
#endif
}

FILE *openFile(const string &path, const char *mode) {
#ifdef _WIN32
    return _wfopen(wide(path).c_str(), wide(mode).c_str());
#else
    return fopen(path.c_str(), mode);
#endif
}

int seekFile(FILE *f, int64_t offset, int whence) {
#ifdef _WIN32
    return _fseeki64(f, offset, whence);
#else
    return fseeko(f, static_cast<off_t>(offset), whence);
#endif
}

int64_t tellFile(FILE *f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return static_cast<int64_t>(ftello(f));
#endif
}

int64_t fileSize(const string &path) {
    FILE *f = openFile(path, "rb");
    if (!f)
        return -1;
    int64_t size = seekFile(f, 0, SEEK_END) == 0 ? tellFile(f) : -1;
    fclose(f);
    return size;
}

// the whole file, or false when it cannot be read (or is bigger than `limit`)
bool readTextFile(const string &path, string &text, size_t limit = 1024 * 1024) {
    FILE *f = openFile(path, "rb");
    if (!f)
        return false;
    text.clear();
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        text.append(buf, n);
        if (text.size() > limit) {
            fclose(f);
            return false;
        }
    }
    fclose(f);
    return true;
}

bool writeTextFile(const string &path, const string &text) {
    FILE *f = openFile(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    return fclose(f) == 0 && ok;
}

bool copyFile(const string &from, const string &to) {
    FILE *in = openFile(from, "rb");
    if (!in)
        return false;
    FILE *out = openFile(to, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    char buf[65536];
    size_t n;
    bool ok = true;
    while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0)
        ok = fwrite(buf, 1, n, out) == n;
    fclose(in);
    return fclose(out) == 0 && ok;
}

// the scratch folder's name appears in every path removeTree is allowed to delete
const char *TmpName = ".pe_tmp";

// A folder and everything in it, links not followed (a link is removed, never what it points at). Only ever
// used on what this program made inside Apps/.pe_tmp: any other path is refused.
bool removeTree(const string &path) {
    string guard = string("/") + TmpName;
    bool inTmp = path.find(guard + "/") != string::npos ||
                 (path.size() >= guard.size() && path.compare(path.size() - guard.size(), guard.size(), guard) == 0);
    if (!inTmp)
        return false;
    bool ok = true;
    for (const Entry &e : listDir(path)) {
        string child = path + "/" + e.name;
        if (e.isDir)
            ok = removeTree(child) && ok;
        else
            ok = removeFile(child) && ok;
    }
    return removeEmptyDir(path) && ok;
}

string lower(string s) {
    for (char &c : s)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

bool endsWith(const string &s, const string &suffix) {
    return s.size() >= suffix.size() && lower(s.substr(s.size() - suffix.size())) == suffix;
}

bool startsWith(const string &s, const string &prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

string fileName(const string &path) {
    size_t slash = path.find_last_of("/\\");
    return slash == string::npos ? path : path.substr(slash + 1);
}

string dirName(const string &path) {
    size_t slash = path.find_last_of("/\\");
    return slash == string::npos ? "." : path.substr(0, slash);
}

string withoutSlash(string path) {
    while (path.size() > 1 && (path.back() == '/' || path.back() == '\\'))
        path.pop_back();
    return path;
}

string trim(const string &s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

// the lines of a text, any of LF, CRLF or CR ending them
vector<string> splitLines(const string &text) {
    vector<string> lines;
    string line;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
            lines.push_back(line);
            line.clear();
        } else {
            line += c;
        }
    }
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

//******************
// text that reaches the user
//******************
// A value that is shown (a title, an author, the readme): no control characters, and the product the packages
// were made for is called "PE" - its name does not appear in anything this program writes.
string shown(const string &raw, size_t limit) {
    string s;
    for (char c : raw)
        s += (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) ? ' ' : c;
    for (const char *name : {"project eris", "project_eris"}) {
        size_t at;
        while ((at = lower(s).find(name)) != string::npos)
            s.replace(at, strlen(name), "PE");
    }
    s = trim(s);
    if (s.size() > limit)
        s.resize(limit);
    return s;
}

//******************
// the shell-style files: launcher.cfg, and the compatibility list
//******************
// `name="value"` lines, read as data and never run: CRLF, blanks and #comments are tolerated, a value is in
// double or single quotes or bare, and nothing in it is expanded.
map<string, string> parseShellVars(const string &text) {
    map<string, string> vars;
    for (string line : splitLines(text)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        if (startsWith(line, "export ") || startsWith(line, "export\t"))
            line = trim(line.substr(7));
        size_t eq = line.find('=');
        if (eq == string::npos || eq == 0)
            continue;
        string key = trim(line.substr(0, eq));
        bool keyOk = !key.empty();
        for (char c : key)
            keyOk = keyOk && (isalnum(static_cast<unsigned char>(c)) || c == '_');
        if (!keyOk)
            continue;
        string value = trim(line.substr(eq + 1));
        if (!value.empty() && (value[0] == '"' || value[0] == '\'')) {
            char quote = value[0];
            size_t end = value.rfind(quote);
            string inner = end > 0 ? value.substr(1, end - 1) : value.substr(1);
            if (quote == '"') { // \" and \\ only
                string un;
                for (size_t i = 0; i < inner.size(); ++i) {
                    if (inner[i] == '\\' && i + 1 < inner.size() && (inner[i + 1] == '"' || inner[i + 1] == '\\'))
                        ++i;
                    un += inner[i];
                }
                inner = un;
            }
            value = inner;
        } else {
            size_t space = value.find_first_of(" \t");
            if (space != string::npos)
                value.resize(space);
        }
        vars[key] = value;
    }
    return vars;
}

struct CompatRule {
    bool block = false;
    bool skip = false;
    string reason;
    string pad; // the App's PadMode (a launcher's default pad identity)
    // the App's Dpad2Analog= / Analog2Dpad= ("1" / "0"; "" = not set): the d-pad also moves the stick / the stick
    // also presses the d-pad
    string dpad2analog;
    string analog2dpad;
};

// "1" / "0" for a value that says on / off, else ""
string flagValue(const string &value) {
    string v = lower(value);
    if (v == "1" || v == "true" || v == "yes" || v == "on")
        return "1";
    if (v == "0" || v == "false" || v == "no" || v == "off")
        return "0";
    return "";
}

// [section] / key=value, section names lower-cased
map<string, CompatRule> parseCompat(const string &text) {
    map<string, CompatRule> rules;
    CompatRule *current = nullptr;
    for (string line : splitLines(text)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (line[0] == '[' && line.back() == ']') {
            current = &rules[lower(trim(line.substr(1, line.size() - 2)))];
            continue;
        }
        size_t eq = line.find('=');
        if (!current || eq == string::npos)
            continue;
        string key = lower(trim(line.substr(0, eq)));
        string value = trim(line.substr(eq + 1));
        if (key == "block")
            current->block = value == "1";
        else if (key == "skip")
            current->skip = value == "1";
        else if (key == "reason")
            current->reason = value;
        else if (key == "pad")
            current->pad = lower(value);
        else if (key == "dpad2analog")
            current->dpad2analog = flagValue(value);
        else if (key == "analog2dpad")
            current->analog2dpad = flagValue(value);
    }
    return rules;
}

map<string, CompatRule> loadCompat(const string &explicitPath) {
    vector<string> candidates;
    if (!explicitPath.empty())
        candidates.push_back(explicitPath);
    if (const char *env = getenv("AB_PE_COMPAT"))
        candidates.push_back(env);
    if (const char *root = getenv("AB_ROOT")) {
        candidates.push_back(string(root) + "/rc/pe_compat.ini");
        candidates.push_back(string(root) + "/Autobleem/rc/pe_compat.ini");
    }
    for (const string &path : candidates) {
        string text;
        if (!path.empty() && readTextFile(path, text))
            return parseCompat(text);
    }
    return parseCompat(BuiltInCompat);
}

//******************
// the package: an ar archive
//******************
struct ArMember {
    string name;
    int64_t offset; // of the data
    int64_t size;
};

// the members of the ar archive in `f`: false when it is not one
bool readAr(FILE *f, int64_t total, vector<ArMember> &members) {
    char magic[8];
    if (seekFile(f, 0, SEEK_SET) != 0 || fread(magic, 1, 8, f) != 8 || memcmp(magic, "!<arch>\n", 8) != 0)
        return false;
    int64_t pos = 8;
    while (pos + 60 <= total) {
        char h[60];
        if (seekFile(f, pos, SEEK_SET) != 0 || fread(h, 1, 60, f) != 60 || h[58] != '`' || h[59] != '\n')
            return false;
        string name(h, 16);
        while (!name.empty() && (name.back() == ' ' || name.back() == '/'))
            name.pop_back();
        string sizeText(h + 48, 10);
        int64_t size = 0;
        for (char c : trim(sizeText)) {
            if (!isdigit(static_cast<unsigned char>(c)))
                return false;
            size = size * 10 + (c - '0');
        }
        pos += 60;
        if (size < 0 || pos + size > total)
            return false;
        members.push_back({name, pos, size});
        pos += size + (size & 1);
    }
    return true;
}

const ArMember *findMember(const vector<ArMember> &members, const string &prefix) {
    for (const ArMember &m : members) {
        if (startsWith(m.name, prefix))
            return &m;
    }
    return nullptr;
}

// A gzip stream (header, deflate, trailer) in memory, unpacked with miniz's inflate into at most `limit` bytes.
bool gunzip(const vector<unsigned char> &in, size_t limit, vector<unsigned char> &out) {
    if (in.size() < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8)
        return false;
    unsigned flags = in[3];
    size_t pos = 10;
    if (flags & 4) { // FEXTRA
        if (pos + 2 > in.size())
            return false;
        pos += 2 + (in[pos] | (in[pos + 1] << 8));
    }
    for (unsigned bit : {8u, 16u}) { // FNAME, FCOMMENT: zero-terminated
        if (flags & bit) {
            while (pos < in.size() && in[pos] != 0)
                ++pos;
            ++pos;
        }
    }
    if (flags & 2) // FHCRC
        pos += 2;
    if (pos + 8 > in.size())
        return false;
    out.assign(limit, 0);
    size_t n = tinfl_decompress_mem_to_mem(out.data(), out.size(), in.data() + pos, in.size() - pos - 8, 0);
    if (n == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED)
        return false;
    out.resize(n);
    return true;
}

//******************
// tar.xz / tar through libarchive, from a byte range of a file or from memory
//******************
struct Input {
    FILE *file = nullptr;
    int64_t pos = 0; // in the range, 0 .. size
    int64_t size = 0;
    int64_t start = 0;
    const vector<unsigned char> *memory = nullptr;
    vector<unsigned char> buffer = vector<unsigned char>(65536);
    function<void(int64_t)> onProgress;
};

class TarReader {
public:
    explicit TarReader(Input &input) : in(input) {
        a = archive_read_new();
        archive_read_support_filter_xz(a);
        archive_read_support_format_tar(a);
        ab_archive_names_utf8(a);
        archive_read_set_read_callback(a, onRead);
        archive_read_set_skip_callback(a, onSkip);
        archive_read_set_callback_data(a, &in);
        if (archive_read_open1(a) != ARCHIVE_OK) {
            why = failure("not a tar archive this program can read");
            archive_read_free(a);
            a = nullptr;
        }
    }
    ~TarReader() {
        if (a)
            archive_read_free(a);
    }
    TarReader(const TarReader &) = delete;
    TarReader &operator=(const TarReader &) = delete;

    archive *a = nullptr;
    string why;

    // what went wrong, in libarchive's words when it has some
    string failure(const string &what) const {
        const char *detail = a ? archive_error_string(a) : nullptr;
        return detail ? what + " (" + detail + ")" : what;
    }

private:
    Input &in;

    static la_ssize_t onRead(archive *, void *data, const void **buf) {
        Input *i = static_cast<Input *>(data);
        int64_t left = i->size - i->pos;
        if (left <= 0)
            return 0;
        size_t want = static_cast<size_t>(min<int64_t>(left, static_cast<int64_t>(i->buffer.size())));
        if (i->memory) {
            *buf = i->memory->data() + i->pos;
            i->pos += static_cast<int64_t>(want);
            return static_cast<la_ssize_t>(want);
        }
        if (seekFile(i->file, i->start + i->pos, SEEK_SET) != 0)
            return -1;
        size_t n = fread(i->buffer.data(), 1, want, i->file);
        if (n == 0)
            return -1;
        *buf = i->buffer.data();
        i->pos += static_cast<int64_t>(n);
        if (i->onProgress)
            i->onProgress(i->pos);
        return static_cast<la_ssize_t>(n);
    }
    static la_int64_t onSkip(archive *, void *data, la_int64_t request) {
        Input *i = static_cast<Input *>(data);
        int64_t n = min<int64_t>(request, i->size - i->pos);
        i->pos += n;
        if (i->onProgress && !i->memory)
            i->onProgress(i->pos);
        return n;
    }
};

// the entry's names, UTF-8
string entryPath(archive_entry *e) {
#ifdef _WIN32
    const char *p = archive_entry_pathname_utf8(e);
#else
    const char *p = archive_entry_pathname(e); // UTF-8 already (ab_archive_names_utf8)
#endif
    return p ? p : "";
}

string entryHardlink(archive_entry *e) {
#ifdef _WIN32
    const char *p = archive_entry_hardlink_utf8(e);
#else
    const char *p = archive_entry_hardlink(e);
#endif
    return p ? p : "";
}

string entrySymlink(archive_entry *e) {
#ifdef _WIN32
    const char *p = archive_entry_symlink_utf8(e);
#else
    const char *p = archive_entry_symlink(e);
#endif
    return p ? p : "";
}

//******************
// names inside the package
//******************
// A package path made plain: "./a//b/" -> "a/b". False for a name that could write outside the folder it is
// unpacked to (absolute, a ".." part, a backslash or a drive colon) or is absurdly long.
bool plainPath(const string &raw, string &out) {
    if (raw.empty() || raw[0] == '/' || raw.find('\\') != string::npos || raw.find(':') != string::npos ||
        raw.size() > MaxPathLength)
        return false;
    out.clear();
    size_t start = 0;
    while (start <= raw.size()) {
        size_t slash = raw.find('/', start);
        string part = raw.substr(start, slash == string::npos ? string::npos : slash - start);
        if (part == "..")
            return false;
        if (part.size() > MaxNameLength)
            return false;
        if (!part.empty() && part != ".")
            out += (out.empty() ? "" : "/") + part;
        if (slash == string::npos)
            break;
        start = slash + 1;
    }
    return true;
}

// `target` of a link at `rel` (both inside one launcher folder, '/' separated) -> the target's own path inside
// the folder; false when it leaves the folder
bool resolveInside(const string &rel, const string &target, string &out) {
    if (target.empty() || target[0] == '/' || target.find('\\') != string::npos)
        return false;
    vector<string> parts;
    size_t start = 0;
    string base = dirName(rel) == "." ? "" : dirName(rel);
    string full = (base.empty() ? "" : base + "/") + target;
    while (start <= full.size()) {
        size_t slash = full.find('/', start);
        string part = full.substr(start, slash == string::npos ? string::npos : slash - start);
        if (part == "..") {
            if (parts.empty())
                return false;
            parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (slash == string::npos)
            break;
        start = slash + 1;
    }
    out.clear();
    for (const string &p : parts)
        out += (out.empty() ? "" : "/") + p;
    return !out.empty();
}

//******************
// the control file
//******************
struct Control {
    string version;
    string maintainer;
    string type;
    string title;               // the first line of Description
    vector<string> description; // the lines after it (continuation lines, ' .' as an empty line)
};

Control parseControl(const string &text) {
    Control c;
    string key;
    for (const string &line : splitLines(text)) {
        if (line.empty())
            continue;
        if (line[0] == ' ' || line[0] == '\t') {
            string body = trim(line);
            if (key == "description") {
                c.description.push_back(body == "." ? "" : body);
                // "Type: USB_MOD" is a continuation line of the Description
                if (startsWith(lower(body), "type:"))
                    c.type = trim(body.substr(5));
            }
            continue;
        }
        size_t colon = line.find(':');
        if (colon == string::npos)
            continue;
        key = lower(line.substr(0, colon));
        string value = trim(line.substr(colon + 1));
        if (key == "version" && c.version.empty())
            c.version = value;
        else if (key == "maintainer")
            c.maintainer = value;
        else if (key == "type")
            c.type = value;
        else if (key == "description")
            c.title = value;
    }
    return c;
}

// the control file out of a package's ar members: false and `why` when there is none
bool readControl(FILE *f, const vector<ArMember> &members, Control &control, string &why) {
    const ArMember *m = findMember(members, "control.tar");
    if (!m) {
        why = "not a PE app package (no control archive)";
        return false;
    }
    string tarName = lower(m->name);
    vector<unsigned char> raw;
    Input in;
    in.file = f;
    in.start = m->offset;
    in.size = m->size;
    vector<unsigned char> plain;
    if (endsWith(tarName, ".gz")) {
        if (static_cast<size_t>(m->size) > MaxMemberInMemory) {
            why = "its control archive is too big";
            return false;
        }
        raw.resize(static_cast<size_t>(m->size));
        if (seekFile(f, m->offset, SEEK_SET) != 0 || fread(raw.data(), 1, raw.size(), f) != raw.size() ||
            !gunzip(raw, MaxMemberInMemory, plain)) {
            why = "its control archive cannot be unpacked";
            return false;
        }
        in.memory = &plain;
        in.size = static_cast<int64_t>(plain.size());
    } else if (!endsWith(tarName, ".xz") && !endsWith(tarName, ".tar")) {
        why = "its control archive is packed in a way this program does not read";
        return false;
    }
    TarReader reader(in);
    if (!reader.a) {
        why = reader.why;
        return false;
    }
    archive_entry *entry;
    int r;
    while ((r = archive_read_next_header(reader.a, &entry)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        string name;
        if (!plainPath(entryPath(entry), name) || name != "control" || archive_entry_filetype(entry) != AE_IFREG)
            continue;
        string text;
        const void *buf;
        size_t n;
        la_int64_t offset;
        while (archive_read_data_block(reader.a, &buf, &n, &offset) == ARCHIVE_OK) {
            text.append(static_cast<const char *>(buf), n);
            if (text.size() > 256 * 1024) {
                why = "its control file is too big";
                return false;
            }
        }
        control = parseControl(text);
        if (control.version.empty()) {
            why = "its control file has no Version";
            return false;
        }
        return true;
    }
    why = "it has no control file";
    return false;
}

// Debian's version order, the part that matters here: digits compare as numbers, letters as letters, '~' first
int compareVersions(const string &a, const string &b) {
    auto order = [](char c) {
        if (c == '~')
            return -1;
        if (isalpha(static_cast<unsigned char>(c)))
            return static_cast<int>(c);
        return static_cast<int>(c) + 256;
    };
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        while ((i < a.size() && !isdigit(static_cast<unsigned char>(a[i]))) ||
               (j < b.size() && !isdigit(static_cast<unsigned char>(b[j])))) {
            int ca = (i < a.size() && !isdigit(static_cast<unsigned char>(a[i]))) ? order(a[i]) : 0;
            int cb = (j < b.size() && !isdigit(static_cast<unsigned char>(b[j]))) ? order(b[j]) : 0;
            if (ca != cb)
                return ca < cb ? -1 : 1;
            if (i < a.size() && !isdigit(static_cast<unsigned char>(a[i])))
                ++i;
            if (j < b.size() && !isdigit(static_cast<unsigned char>(b[j])))
                ++j;
        }
        while (i < a.size() && a[i] == '0')
            ++i;
        while (j < b.size() && b[j] == '0')
            ++j;
        size_t si = i, sj = j;
        while (i < a.size() && isdigit(static_cast<unsigned char>(a[i])))
            ++i;
        while (j < b.size() && isdigit(static_cast<unsigned char>(b[j])))
            ++j;
        if (i - si != j - sj)
            return i - si < j - sj ? -1 : 1;
        int c = a.compare(si, i - si, b, sj, j - sj);
        if (c != 0)
            return c < 0 ? -1 : 1;
    }
    return 0;
}

//******************
// the Apps folder: what a converted package leaves
//******************
struct Paths {
    string apps;
    string tmp;   // Apps/.pe_tmp: unpacking and replacing happen here, on the same filesystem as Apps
    string state; // Apps/.pe_state: one small marker per package
};

Paths pathsFor(const string &apps) {
    return {apps, apps + "/" + TmpName, apps + "/.pe_state"};
}

// A killed run left unpacked folders in .pe_tmp (cleared) - and maybe a folder it had moved aside to replace
// it, with nothing in its place (put back).
void recover(const Paths &p) {
    for (const Entry &e : listDir(p.tmp)) {
        string path = p.tmp + "/" + e.name;
        if (e.isDir && endsWith(e.name, ".old")) {
            string final = p.apps + "/" + e.name.substr(0, e.name.size() - 4);
            if (!exists(final))
                renameFile(path, final);
        }
    }
    removeTree(p.tmp);
}

struct AppIni {
    map<string, string> keys; // lower-cased names
    string get(const string &k) const {
        auto it = keys.find(k);
        return it == keys.end() ? "" : it->second;
    }
};

AppIni readAppIni(const string &folder) {
    AppIni ini;
    string text;
    if (!readTextFile(folder + "/app.ini", text))
        return ini;
    for (const string &line : splitLines(text)) {
        size_t eq = line.find('=');
        if (eq != string::npos && !line.empty() && line[0] != ';' && line[0] != '#')
            ini.keys[lower(trim(line.substr(0, eq)))] = trim(line.substr(eq + 1));
    }
    return ini;
}

struct Marker {
    string version;
    int64_t size = -1;
    vector<string> apps;
};

string markerPath(const Paths &p, const string &modName) {
    return p.state + "/" + modName + ".ini";
}

Marker readMarker(const Paths &p, const string &modName) {
    Marker m;
    string text;
    if (!readTextFile(markerPath(p, modName), text, 65536))
        return m;
    for (const string &line : splitLines(text)) {
        size_t eq = line.find('=');
        if (eq == string::npos)
            continue;
        string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "Version")
            m.version = v;
        else if (k == "Size")
            m.size = atoll(v.c_str());
        else if (k == "Apps") {
            size_t start = 0;
            while (start <= v.size()) {
                size_t comma = v.find(',', start);
                string name = v.substr(start, comma == string::npos ? string::npos : comma - start);
                if (!name.empty())
                    m.apps.push_back(name);
                if (comma == string::npos)
                    break;
                start = comma + 1;
            }
        }
    }
    return m;
}

void writeMarker(const Paths &p, const string &modName, const Marker &m) {
    string apps;
    for (const string &a : m.apps)
        apps += (apps.empty() ? "" : ",") + a;
    makeDirs(p.state);
    writeTextFile(markerPath(p, modName),
                  "Version=" + m.version + "\nSize=" + to_string(m.size) + "\nApps=" + apps + "\n");
}

// The package is already converted: the same file size and Version as the marker says, and each of its Apps
// still there with this package as PeSource and this Version.
bool alreadyConverted(const Paths &p, const string &modName, int64_t size, const string &version) {
    Marker m = readMarker(p, modName);
    if (m.version != version || m.size != size)
        return false;
    for (const string &app : m.apps) {
        AppIni ini = readAppIni(p.apps + "/" + app);
        if (ini.get("pesource") != modName || ini.get("version") != version)
            return false;
    }
    return true;
}

// what the old folder had that the new one does not: moved over, at any depth. A file both have is the new
// one's. Nothing of the old folder is lost this way except what the new package replaces.
void mergeOld(const string &oldDir, const string &newDir) {
    for (const Entry &e : listDir(oldDir)) {
        string from = oldDir + "/" + e.name, to = newDir + "/" + e.name;
        if (!exists(to))
            renameFile(from, to);
        else if (e.isDir && isDir(to))
            mergeOld(from, to);
    }
}

// the staged folder into Apps/<name>: renamed in when new, or merged with the old one first; false on failure
bool installFolder(const Paths &p, const string &staged, const string &name) {
    string final = p.apps + "/" + name;
    if (!exists(final))
        return renameFile(staged, final);
    string old = p.tmp + "/" + name + ".old";
    if (!renameFile(final, old))
        return false;
    // the user's own settings files are theirs even if the new package ships a file of that name
    for (const char *keep : {"ab_settings.ini", "pad.ini"}) {
        if (exists(old + "/" + keep))
            removeFile(staged + "/" + keep);
    }
    mergeOld(old, staged);
    if (!renameFile(staged, final)) {
        renameFile(old, final); // back as it was
        return false;
    }
    removeTree(old);
    return true;
}

//******************
// converting one package
//******************
// One launcher folder met while the data is unpacked, in Apps/.pe_tmp/<package>/<dir>/
struct Launcher {
    string dir;
    string staged;
    bool hasCfg = false;
    bool hasLaunch = false;
    string cfg;
    uint64_t bytes = 0;
    struct Link {
        string rel, target; // target: inside the folder
        bool hard;
    };
    vector<Link> links;
};

// the shown value of a key of launcher.cfg, or `fallback`
string cfgValue(const map<string, string> &cfg, const string &key, const string &fallback = "") {
    auto it = cfg.find(key);
    return it == cfg.end() ? fallback : it->second;
}

bool validFilename(const string &s) {
    if (s.empty() || s.size() > 64)
        return false;
    for (char c : s) {
        if (!(islower(static_cast<unsigned char>(c)) || isdigit(static_cast<unsigned char>(c))))
            return false;
    }
    return true;
}

// the Maintainer without its address: "Name <someone@example>" -> "Name"
string maintainerName(const string &m) {
    size_t lt = m.find('<');
    return trim(lt == string::npos ? m : m.substr(0, lt));
}

string readmeText(const Control &c) {
    string text = shown(c.title, 200) + "\n";
    size_t lines = 0;
    for (string raw : c.description) {
        // the packers' Makefile appends " Author: <name>" straight after a description that has no trailing
        // newline: the tail is the metadata line that slipped into the text
        size_t glued = raw.find(" Author:");
        if (glued != string::npos)
            raw = trim(raw.substr(0, glued));
        if (glued != string::npos && raw.empty())
            continue;
        string l = lower(raw);
        bool meta = false;
        for (const char *k : {"type:", "author:", "platform:", "git commit:", "built:"})
            meta = meta || startsWith(l, k);
        if (meta)
            continue;
        string line = raw.empty() ? "" : shown(raw, 300);
        if (line.empty() && (lines == 0 || text.size() > 1800))
            continue;
        text += line + "\n";
        ++lines;
        if (text.size() > 2000)
            break;
    }
    text += "\nPut the files this program needs (game data) in this folder.\n";
    text += "Compatibility mode: this is a third-party program that runs its own script; it may not work fully.\n";
    return text;
}

struct Result {
    bool ok = false;
    string why;
    vector<string> apps; // the folders (pe-...) this package's Apps are in
    vector<string> replacedSources; // the other packages whose App this one replaced (their .mod is retired)
};

// Converts the package at `modPath`. A launcher folder that is refused (listed, hybrid, installed already) is a
// #WARN and not a failure; a package that is damaged or unsafe is `ok == false`.
Result convert(const Paths &p, const map<string, CompatRule> &compat, const string &modPath, bool progressLines) {
    Result result;
    const string modName = fileName(modPath);
    FILE *f = openFile(modPath, "rb");
    if (!f) {
        result.why = "cannot be opened";
        return result;
    }
    unique_ptr<FILE, int (*)(FILE *)> closer(f, fclose);
    int64_t total = fileSize(modPath);

    vector<ArMember> members;
    if (!readAr(f, total, members)) {
        result.why = "not a PE app package (not an ar archive)";
        return result;
    }
    Control control;
    if (!readControl(f, members, control, result.why))
        return result;
    control.version = shown(control.version, 60);
    if (!control.type.empty() && control.type != "USB_MOD") {
        result.why = "not a PE app package (Type " + shown(control.type, 40) + ")";
        return result;
    }
    if (alreadyConverted(p, modName, total, control.version)) {
        result.ok = true;
        return result;
    }
    const ArMember *data = findMember(members, "data.tar");
    if (!data || !endsWith(data->name, ".xz")) {
        result.why = "its data is packed in a way this program does not read";
        return result;
    }
    if (!makeDirs(p.tmp)) {
        result.why = "could not create " + p.tmp;
        return result;
    }

    // ---- one pass over the data: the launcher folders, unpacked as they come
    const string stageRoot = p.tmp + "/pkg";
    removeTree(stageRoot);
    auto fail = [&](const string &why) {
        result.why = why;
        removeTree(stageRoot);
        return result;
    };
    int lastPercent = -1;
    Input in;
    in.file = f;
    in.start = data->offset;
    in.size = data->size;
    if (progressLines) {
        in.onProgress = [&](int64_t pos) {
            int percent = in.size == 0 ? 100 : static_cast<int>(pos * 100 / in.size);
            if (percent != lastPercent) {
                lastPercent = percent;
                say(to_string(percent));
            }
        };
    }
    TarReader reader(in);
    if (!reader.a)
        return fail(reader.why);

    map<string, Launcher> launchers;
    uint64_t packageBytes = 0;
    size_t entries = 0;
    const string prefix = LauncherPrefix;
    archive_entry *entry;
    int r;
    while ((r = archive_read_next_header(reader.a, &entry)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        if (++entries > MaxEntries)
            return fail("too many files in it");
        string path;
        if (!plainPath(entryPath(entry), path))
            return fail("unsafe name in the package: " + shown(entryPath(entry), 100));
        string hard = entryHardlink(entry);
        string sym = entrySymlink(entry);
        if (!hard.empty()) {
            string h;
            if (!plainPath(hard, h))
                return fail("unsafe link in the package: " + shown(hard, 100));
        }
        if (!startsWith(path, prefix))
            continue; // anything outside the launcher folders is not ours to unpack
        string inside = path.substr(prefix.size());
        size_t slash = inside.find('/');
        string dir = inside.substr(0, slash);
        string rel = slash == string::npos ? "" : inside.substr(slash + 1);
        if (dir.empty())
            continue;
        Launcher &l = launchers[dir];
        if (l.dir.empty()) {
            l.dir = dir;
            l.staged = stageRoot + "/" + dir;
        }
        if (rel.empty()) {
            if (!makeDirs(l.staged))
                return fail("could not create " + l.staged);
            continue;
        }
        const string target = l.staged + "/" + rel;
        unsigned filetype = archive_entry_filetype(entry);
        if (!sym.empty() || !hard.empty()) {
            string to;
            bool inside2;
            if (!sym.empty()) {
                inside2 = resolveInside(rel, sym, to);
            } else {
                string h;
                plainPath(hard, h);
                inside2 = startsWith(h, prefix + dir + "/");
                to = inside2 ? h.substr(prefix.size() + dir.size() + 1) : "";
            }
            if (!inside2 || to.empty())
                return fail("a link in the package points outside its folder: " + shown(rel, 100));
            l.links.push_back({rel, to, !hard.empty()});
            continue;
        }
        if (filetype == AE_IFDIR) {
            if (!makeDirs(target))
                return fail("could not create " + target);
            continue;
        }
        if (filetype != AE_IFREG)
            continue; // a device or a pipe: not unpacked
        uint64_t declared = archive_entry_size_is_set(entry) ? static_cast<uint64_t>(archive_entry_size(entry)) : 0;
        if (declared > MaxFileSize)
            return fail("a file in the package is too big: " + shown(rel, 100));
        if (rel == "launcher.cfg")
            l.hasCfg = true;
        if (rel == "launch.sh")
            l.hasLaunch = true;
        if (!makeDirs(dirName(target)))
            return fail("could not create " + dirName(target));
        FILE *out = openFile(target, "wb");
        if (!out)
            return fail("could not write " + shown(rel, 100));
        const void *buf;
        size_t n;
        la_int64_t offset;
        uint64_t written = 0;
        bool ok = true;
        while ((r = archive_read_data_block(reader.a, &buf, &n, &offset)) == ARCHIVE_OK) {
            written += n;
            packageBytes += n;
            if (written > MaxFileSize || packageBytes > MaxPackageSize) {
                ok = false;
                break;
            }
            if (rel == "launcher.cfg" && l.cfg.size() < 65536)
                l.cfg.append(static_cast<const char *>(buf), n);
            if (fwrite(buf, 1, n, out) != n) {
                ok = false;
                break;
            }
        }
        bool closed = fclose(out) == 0;
        if (!ok || !closed || r != ARCHIVE_EOF) {
            if (written > MaxFileSize || packageBytes > MaxPackageSize)
                return fail("the package is too big");
            return fail(reader.failure("could not unpack " + shown(rel, 100)) +
                        " - a damaged package, or the stick is full");
        }
        l.bytes += written;
        setMode(target, archive_entry_mode(entry));
    }
    if (r != ARCHIVE_EOF)
        return fail(reader.failure("it cannot be read") + " - a damaged package");

    // ---- each launcher folder: the cfg, the list, the folder's place in Apps
    set<string> taken;
    for (auto &kv : launchers) {
        Launcher &l = kv.second;
        if (!l.hasCfg || !l.hasLaunch) {
            say("#WARN - " + modName + ": " + shown(l.dir, 60) +
                " is not a launcher folder (launcher.cfg or launch.sh missing)");
            continue;
        }
        map<string, string> cfg = parseShellVars(l.cfg);
        string fn = cfgValue(cfg, "launcher_filename");
        string title = shown(cfgValue(cfg, "launcher_title", fn), 120);
        if (title.empty())
            title = fn;
        if (!validFilename(fn)) {
            say("#WARN - " + modName + ": " + shown(l.dir, 60) + " not added: its launcher_filename is not usable");
            continue;
        }
        // the list: by the cfg's name, the folder's own name, and that without underscores
        string dirPlain;
        for (char c : lower(l.dir))
            if (c != '_')
                dirPlain += c;
        const CompatRule *rule = nullptr;
        for (const string &name : {fn, lower(l.dir), dirPlain}) {
            auto it = compat.find(name);
            if (it != compat.end() && (it->second.block || it->second.skip)) {
                rule = &it->second;
                break;
            }
        }
        // the pad mode: the first section (cfg name, folder name, folder name without underscores) that has one
        string pad;
        for (const string &name : {fn, lower(l.dir), dirPlain}) {
            auto it = compat.find(name);
            if (it != compat.end() && !it->second.pad.empty()) {
                pad = it->second.pad;
                break;
            }
        }
        // the d-pad / stick flags the same way, each on its own (none: the launcher's default for the pad output)
        string dpad2analog, analog2dpad;
        for (const string &name : {fn, lower(l.dir), dirPlain}) {
            auto it = compat.find(name);
            if (it == compat.end())
                continue;
            if (dpad2analog.empty())
                dpad2analog = it->second.dpad2analog;
            if (analog2dpad.empty())
                analog2dpad = it->second.analog2dpad;
        }
        if (pad.empty())
            pad = DefaultPadMode;
        else if (pad != "psc" && pad != "x360" && pad != "psc-kernel" && pad != "x360-kernel") {
            say("#WARN - " + modName + ": " + title + ": pad mode " + shown(pad, 30) + " is not known, using " +
                DefaultPadMode);
            pad = DefaultPadMode;
        }
        if (rule) {
            string reason =
                rule->reason.empty() ? (rule->block ? "it must not run here" : "not used here") : rule->reason;
            say("#WARN - " + modName + ": " + title + " not added (" + shown(reason, 120) + ")");
            continue;
        }
        const string name = "pe-" + fn;
        if (!taken.insert(name).second) {
            say("#WARN - " + modName + ": " + title + " not added (a second folder with the same name)");
            continue;
        }

        // an App of this name that is already there: ours (PeSource) and older is replaced; anything else stays
        const string final = p.apps + "/" + name;
        if (exists(final)) {
            AppIni have = readAppIni(final);
            if (have.get("pesource").empty()) {
                say("#WARN - " + modName + ": " + title + " not added (Apps/" + name +
                    " exists and is not from a package)");
                continue;
            }
            int cmp = compareVersions(control.version, have.get("version"));
            if (cmp < 0) {
                say("#WARN - " + modName + ": " + title + " not added (a newer version, " +
                    shown(have.get("version"), 40) + ", is installed)");
                continue;
            }
            // the same version from another file: nothing to do (and not this package's App); the same file
            // changed under the same version is made again
            if (cmp == 0 && have.get("pesource") != modName)
                continue;
            if (have.get("pesource") != modName)
                result.replacedSources.push_back(have.get("pesource"));
        }

        // links become copies (the stick is FAT)
        bool linksOk = true;
        for (const Launcher::Link &link : l.links) {
            string from = l.staged + "/" + link.target, to = l.staged + "/" + link.rel;
            if (!exists(from) || isDir(from)) {
                say("#WARN - " + modName + ": " + title + ": a link to " + shown(link.target, 80) + " was left out");
                continue;
            }
            if (exists(to) || !makeDirs(dirName(to)) || !copyFile(from, to)) {
                linksOk = false;
                break;
            }
        }
        if (!linksOk)
            return fail("could not copy a link in " + shown(l.dir, 60));

        // the generated files (they win over a file of the same name in the folder)
        string author = shown(cfgValue(cfg, "launcher_publisher"), 120);
        if (author.empty())
            author = shown(maintainerName(control.maintainer), 120);
        string ini = "Title=" + title + "\nAuthor=" + author + "\nVersion=" + shown(control.version, 60) + "\n";
        if (exists(l.staged + "/" + fn + ".png"))
            ini += "Image=" + fn + ".png\n";
        ini += "Readme=readme.txt\nStartup=run.sh\nExec.psc=run.sh\nCategory=PE\nPeSource=" + modName +
               "\nPadMode=" + pad + "\n";
        if (!dpad2analog.empty())
            ini += "Dpad2Analog=" + dpad2analog + "\n";
        if (!analog2dpad.empty())
            ini += "Analog2Dpad=" + analog2dpad + "\n";
        const char *runSh = "#!/bin/sh\n"
                            "# PE App launcher - generated, do not edit\n"
                            "APP_DIR=\"$(cd \"$(dirname \"$0\")\" && pwd)\"\n"
                            "exec sh \"${AB_RC_DIR:-/media/Autobleem/rc}/pe_run.sh\" \"$APP_DIR\"\n";
        removeFile(l.staged + "/run.sh");
        if (!writeTextFile(l.staged + "/app.ini", ini) ||
            !writeTextFile(l.staged + "/readme.txt", readmeText(control)) ||
            !writeTextFile(l.staged + "/run.sh", runSh))
            return fail("could not write the files of " + title + " - the stick may be full");
        setMode(l.staged + "/run.sh", 0755);

        say("#Adding " + title);
        if (!installFolder(p, l.staged, name))
            return fail("could not put " + title + " in Apps");
        result.apps.push_back(name);
    }

    removeTree(stageRoot);
    Marker m;
    m.version = control.version;
    m.size = total;
    m.apps = result.apps;
    writeMarker(p, modName, m);
    result.ok = true;
    return result;
}

//******************
// --start
//******************
// A converted package leaves the Mods folder for Mods/done/ (not deleted: the user can take the original back).
// A copy already in done/ (the same name dropped again, or an older version) is replaced. A failure to move is a
// #WARN only: the App is complete and its marker written, so the next run finds the package converted and tries
// the move again.
void retire(const string &doneDir, const string &modPath, const Result &r) {
    const string name = fileName(modPath);
    if (!makeDirs(doneDir)) {
        say("#WARN - " + name + ": could not create " + doneDir + " - the package stays in Mods");
        return;
    }
    const string to = doneDir + "/" + name;
    removeFile(to);
    if (!renameFile(modPath, to)) {
        say("#WARN - " + name + ": could not move it to " + fileName(doneDir) + " - it stays in Mods");
        return;
    }
    for (const string &old : r.replacedSources) {
        if (old != name && endsWith(old, ".mod") && old.find_first_of("/\\") == string::npos)
            removeFile(doneDir + "/" + old);
    }
}

// `doneDir` is empty for a single package (--mod): that file stays where it is.
int start(const vector<string> &mods, const Paths &paths, const map<string, CompatRule> &compat,
          const string &doneDir) {
    if (!makeDirs(paths.apps)) {
        say("#ERROR - could not create " + paths.apps);
        return 1;
    }
    recover(paths);
    int failed = 0;
    string firstReason;
    for (size_t i = 0; i < mods.size(); ++i) {
        say("#Converting " + fileName(mods[i]));
        say(to_string(i + 1) + "/" + to_string(mods.size()));
        Result r = convert(paths, compat, mods[i], true);
        if (!r.ok) {
            ++failed;
            if (firstReason.empty())
                firstReason = fileName(mods[i]) + ": " + r.why;
            say("#WARN - " + fileName(mods[i]) + ": " + r.why);
        } else if (!doneDir.empty()) {
            retire(doneDir, mods[i], r);
        }
    }
    removeTree(paths.tmp);
    if (failed > 0) {
        say("#ERROR - " + to_string(failed) + " of " + to_string(mods.size()) + " packages could not be converted (" +
            firstReason + ")");
        return 1;
    }
    say("#DONE");
    return 0;
}

bool looksLikePackage(const string &path) {
    if (!endsWith(path, ".mod"))
        return false;
    FILE *f = openFile(path, "rb");
    if (!f)
        return false;
    char magic[8];
    bool ok = fread(magic, 1, 8, f) == 8 && memcmp(magic, "!<arch>\n", 8) == 0;
    fclose(f);
    return ok;
}

int usage() {
    fprintf(stderr, "usage: pe --version | --ismine --mod <file> | --start --mods <dir> [--apps <dir>]"
                    " | --start --mod <file> [--apps <dir>]  [--compat <file>]\n");
    return 2;
}

} // namespace

int main(int argc, char **argv) {
    vector<string> args(argv + 1, argv + argc);
    auto value = [&args](const string &flag) {
        for (size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == flag)
                return withoutSlash(args[i + 1]);
        }
        return string();
    };
    auto has = [&args](const string &flag) {
        for (const string &a : args) {
            if (a == flag)
                return true;
        }
        return false;
    };

    const string title = string("PE app packages V") + Version;
    if (has("--version")) {
        say("#" + title + " - " + Description);
        return 0;
    }
    if (has("--ismine")) {
        string file = has("--mod") ? value("--mod") : value("--rom");
        return !file.empty() && looksLikePackage(file) ? 0 : 1;
    }
    if (has("--start")) {
        say("#Starting - " + title);
        string appsDir = value("--apps");
        if (appsDir.empty() && getenv("AB_APPS_DIR"))
            appsDir = withoutSlash(getenv("AB_APPS_DIR"));
        vector<string> mods;
        string doneDir;
        if (has("--mods")) {
            string dir = value("--mods");
            doneDir = dir + "/done";
            if (appsDir.empty())
                appsDir = (dirName(dir) == "." && dir.find('/') == string::npos ? "." : dirName(dir)) + "/Apps";
            for (const Entry &e : listDir(dir)) {
                if (!e.isDir && e.name[0] != '.' && endsWith(e.name, ".mod"))
                    mods.push_back(dir + "/" + e.name);
            }
        } else if (has("--mod")) {
            string file = value("--mod");
            if (appsDir.empty())
                appsDir = dirName(dirName(file)) + "/Apps";
            if (looksLikePackage(file))
                mods.push_back(file);
        } else {
            say("#DONE"); // a kind it does not do: nothing to do
            return 0;
        }
        if (mods.empty()) {
            say("#DONE");
            return 0;
        }
        return start(mods, pathsFor(appsDir), loadCompat(value("--compat")), doneDir);
    }
    return usage();
}
