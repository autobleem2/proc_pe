/*
 * proc_pe: every name libarchive hands back in UTF-8, whatever the locale.
 *
 * libarchive converts an entry's name (UTF-16 in a 7z, UTF-16 or OEM in a RAR4) to "the current locale"'s
 * character set. The console has no locales at all - it runs in "C", which is ASCII - and this build has no
 * iconv, so every non-ASCII name would come back as '?'. Telling the archive object that its current
 * character set is UTF-8 makes libarchive take its own built-in UTF-16 -> UTF-8 route instead, on every
 * platform: archive_entry_pathname() is then UTF-8 (on Windows, archive_entry_pathname_utf8() is). Must be
 * called before the first archive_read_next_header().
 */
#include "archive_platform.h"

#include <stdlib.h>
#include <string.h>

#include "archive_private.h"
#include "ab_names.h"

void ab_archive_names_utf8(struct archive *a) {
    free(a->current_code);
    a->current_code = strdup("UTF-8");
#if defined(_WIN32) && !defined(__CYGWIN__)
    a->current_codepage = CP_UTF8;
    a->current_oemcp = CP_UTF8;
#endif
}
