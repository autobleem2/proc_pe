/* proc_pe: every name libarchive hands back in UTF-8, whatever the locale (see ab_names.c) */
#ifndef AB_NAMES_H
#define AB_NAMES_H

#ifdef __cplusplus
extern "C" {
#endif

struct archive;
void ab_archive_names_utf8(struct archive *a);

#ifdef __cplusplus
}
#endif

#endif
