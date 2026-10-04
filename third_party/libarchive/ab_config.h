/*
 * proc_pe: libarchive's configuration, written by hand for the read side only (the tar reader and the xz
 * filter) on the targets proc_pe builds for - glibc Linux (the console's Stretch-era 2.24 up) and
 * MinGW-w64. No iconv, no zlib, no bzip2, no crypto library: the xz filter is the vendored liblzma
 * (third_party/xz), everything else the tar reader needs is libarchive's own. Chosen by PLATFORM_CONFIG_H in
 * CMakeLists.
 */
#ifndef AB_LIBARCHIVE_CONFIG_H
#define AB_LIBARCHIVE_CONFIG_H

/* liblzma, vendored (static) */
#define HAVE_LIBLZMA 1
#define HAVE_LZMA_H 1
#define LZMA_API_STATIC 1

/* the C library every target has */
#define HAVE_CTYPE_H 1
#define HAVE_ERRNO_H 1
#define HAVE_EILSEQ 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_LOCALE_H 1
#define HAVE_SETLOCALE 1
#define HAVE_STDARG_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_TIME_H 1
#define HAVE_WCHAR_H 1
#define HAVE_WCHAR_T 1
#define HAVE_WCTYPE_H 1
#define STDC_HEADERS 1

#define HAVE_DECL_INT32_MAX 1
#define HAVE_DECL_INT32_MIN 1
#define HAVE_DECL_INT64_MAX 1
#define HAVE_DECL_INT64_MIN 1
#define HAVE_DECL_INTMAX_MAX 1
#define HAVE_DECL_INTMAX_MIN 1
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_UINT32_MAX 1
#define HAVE_DECL_UINT64_MAX 1
#define HAVE_DECL_UINTMAX_MAX 1
#define HAVE_INTMAX_T 1
#define HAVE_UINTMAX_T 1
#define HAVE_LONG_LONG_INT 1
#define HAVE_UNSIGNED_LONG_LONG 1
#define HAVE_UNSIGNED_LONG_LONG_INT 1

#define HAVE_MBRTOWC 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMSET 1
#define HAVE_STRCHR 1
#define HAVE_STRDUP 1
#define HAVE_STRERROR 1
#define HAVE_STRRCHR 1
#define HAVE_VPRINTF 1
#define HAVE_WCRTOMB 1
#define HAVE_WCSCMP 1
#define HAVE_WCSCPY 1
#define HAVE_WCSLEN 1
#define HAVE_WCTOMB 1
#define HAVE_WMEMCMP 1
#define HAVE_WMEMCPY 1
#define HAVE_WMEMMOVE 1

#if defined(_WIN32)

#define HAVE_WINDOWS_H 1
#define HAVE_WINCRYPT_H 1
#define HAVE_IO_H 1
#define HAVE_DIRECT_H 1
#define HAVE_SYS_UTIME_H 1
#define HAVE__CTIME64_S 1
#define HAVE__FSEEKI64 1
#define HAVE__GET_TIMEZONE 1
#define HAVE__MKGMTIME 1
#define HAVE_DECL_SSIZE_MAX 1
#define HAVE_UNISTD_H 1
#define HAVE_SYS_TIME_H 1
#define SIZEOF_WCHAR_T 2
/* MinGW has no user or group ids */
#define gid_t short
#define uid_t short
#define id_t short

#else

#define _GNU_SOURCE 1
#define HAVE_DECL_SSIZE_MAX 1
#define HAVE_DIRENT_H 1
#define HAVE_FCNTL 1
#define HAVE_FSEEKO 1
#define HAVE_FSTAT 1
#define HAVE_GETPID 1
#define HAVE_GMTIME_R 1
#define HAVE_LANGINFO_H 1
#define HAVE_LOCALTIME_R 1
#define HAVE_LSTAT 1
#define HAVE_NL_LANGINFO 1
#define HAVE_PTHREAD_H 1
#define HAVE_STRERROR_R 1
#define HAVE_DECL_STRERROR_R 1
#define STRERROR_R_CHAR_P 1
#define HAVE_STRINGS_H 1
#define HAVE_STRUCT_TM_TM_GMTOFF 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_TIMEGM 1
#define HAVE_TZSET 1
#define HAVE_UNISTD_H 1
#define SIZEOF_WCHAR_T 4

#endif

#endif
