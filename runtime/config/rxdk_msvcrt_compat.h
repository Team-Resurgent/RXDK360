/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * RXDK-360 MSVC-CRT extension shims for the modern (clang + picolibc) toolchain.
 *
 * The stock XDK/ATG code uses a handful of Microsoft-specific CRT extensions
 * (the underscore-prefixed names) that ISO C / picolibc do not provide. picolibc
 * does provide the POSIX equivalents, so forward to those. Force-included in the
 * modern build so XDK/sample sources resolve them without the XDK's own CRT.
 */
#ifndef RXDK_MSVCRT_COMPAT_H
#define RXDK_MSVCRT_COMPAT_H

/* ---- macros ---- */
#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef _MAX_PATH
#define _MAX_PATH 260
#endif
#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif
/* SAL code-analysis fallthrough marker: the XDK places `__fallthrough` before a
   `case` label with no trailing statement, where clang's real fallthrough
   attribute is rejected. It carries no codegen meaning - make it a no-op.
   picolibc's <sys/cdefs.h> (force-included first, via picolibc.h) defines this
   as the real [[fallthrough]] attribute for its own build; this header is
   force-included after it, so override that definition to the empty marker the
   XDK's usage needs. */
#undef __fallthrough
#define __fallthrough

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>   /* strcasecmp / strncasecmp */
#include <wchar.h>
#include <wctype.h>
#include <alloca.h>
#include <stdlib.h>    /* mbstowcs_s */
#include <malloc.h>    /* memalign (_aligned_malloc) */
#include <time.h>      /* localtime_s */
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- case-insensitive compares ---- */
static inline int _stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
static inline int _strnicmp(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }
static inline int _wcsicmp(const wchar_t *a, const wchar_t *b) { return wcscasecmp(a, b); }
static inline int _wcsnicmp(const wchar_t *a, const wchar_t *b, size_t n) { return wcsncasecmp(a, b, n); }
/* Non-underscore POSIX-name spellings some XDK samples use directly. */
static inline int stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
static inline int strnicmp(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }

/* ---- misc MSVC CRT extras ---- */
static inline char *_strdup(const char *s) { return strdup(s); }
static inline void *_aligned_malloc(size_t size, size_t align) { return memalign(align, size); }
static inline void  _aligned_free(void *p) { free(p); }
/* fopen_s: a macro (not an inline) so it does not force <stdio.h> into this
   force-included header - FILE/fopen resolve at the call site, where the title
   has already chosen its <stdio.h>. */
#define fopen_s(pf, name, mode) ((*(pf) = fopen((name), (mode))) ? 0 : (errno ? errno : 22))
static inline int   mbstowcs_s(size_t *conv, wchar_t *dst, size_t dstsz, const char *src, size_t count)
{ size_t n = mbstowcs(dst, src, count == (size_t)-1 ? dstsz : count);
  if (n == (size_t)-1) return 42 /*EILSEQ*/;
  if (dst && n < dstsz) dst[n] = 0;
  if (conv) *conv = n + 1; return 0; }
static inline int localtime_s(struct tm *result, const time_t *t)
{ return localtime_r(t, result) ? 0 : 22 /*EINVAL*/; }

/* ---- MSVC <ctype.h> classification bit masks (some samples use the raw
        _SPACE/_DIGIT/... bits with _isctype/_pctype). ---- */
#ifndef _UPPER
#define _UPPER     0x1
#define _LOWER     0x2
#define _DIGIT     0x4
#define _SPACE     0x8
#define _PUNCT     0x10
#define _CONTROL   0x20
#define _BLANK     0x40
#define _HEX       0x80
#define _LEADBYTE  0x8000
#define _ALPHA     (0x0100 | _UPPER | _LOWER)
#endif

/* ---- wide numeric conversions ---- */
static inline int       _wtoi(const wchar_t *s)   { return (int)wcstol(s, (wchar_t **)0, 10); }
static inline long long _wtoi64(const wchar_t *s) { return wcstoll(s, (wchar_t **)0, 10); }
static inline double    _wtof(const wchar_t *s)   { return wcstod(s, (wchar_t **)0); }

/* ---- locale-aware numeric conversions (_locale_t ignored: C locale) ---- */
static inline double        _strtod_l(const char *s, char **e, void *loc)              { (void)loc; return strtod(s, e); }
static inline double        _wcstod_l(const wchar_t *s, wchar_t **e, void *loc)         { (void)loc; return wcstod(s, e); }
static inline long          _strtol_l(const char *s, char **e, int b, void *loc)        { (void)loc; return strtol(s, e, b); }
static inline unsigned long _strtoul_l(const char *s, char **e, int b, void *loc)       { (void)loc; return strtoul(s, e, b); }
static inline double        _atof_l(const char *s, void *loc)                           { (void)loc; return strtod(s, (char **)0); }

/* ---- format-length queries ---- */
static inline int _vscprintf(const char *fmt, va_list a) { return vsnprintf((char *)0, 0, fmt, a); }
static inline int _vscwprintf(const wchar_t *fmt, va_list a)
{ wchar_t tmp[4096]; return vswprintf(tmp, sizeof(tmp) / sizeof(tmp[0]), fmt, a); }

/* xnamath.h defines the VMX128 multiply-add-complement macro __vmaddcfp only
   under _XM_VMX128_INTRINSICS_, but we compile the XDK headers with
   _XM_NO_INTRINSICS_ (scalar math path) while still providing __vmaddfp in the
   runtime. Alias it here so VMX128 title code (FastBlockCompress) still builds. */
#ifndef __vmaddcfp
#define __vmaddcfp(VRA, VRC, VRB) __vmaddfp(VRA, VRC, VRB)
#endif

/* clang implements no structured exception handling on this PPC target. The only
   __try/__except in the sample corpus is the debugger "set thread name" helper
   (it RaiseException(0x406D1388)s an info struct the debugger reads, wrapped in
   __try/__except to swallow it when no debugger is attached). That is cosmetic,
   so degrade it safely: SKIP the __try body (the RaiseException, which without a
   handler would propagate) and run the __except body. The __except expression --
   GetExceptionCode()/EXCEPTION_* -- is discarded, so those need not be defined. */
#ifndef __try
#define __try        if (0)
#define __except(e)  else
#define __finally
#define __leave
#endif
#ifndef __noop
#define __noop ((void)0)
#endif

/* ---- MSVC min/max macros (normally from stdlib.h) ---- */
#ifndef __max
#define __max(a,b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef __min
#define __min(a,b) (((a) < (b)) ? (a) : (b))
#endif

/* ---- bounded printf (MS spelling) ---- */
static inline int _vsnprintf_s(char *d, size_t dstsize, size_t count, const char *fmt, va_list a)
{ (void)count; return vsnprintf(d, dstsize, fmt, a); }
/* 4-arg pointer form: _vsnprintf_s(buf, count/_TRUNCATE, fmt, ap) where the size
   is not separately given -- bound by `count` unless it is _TRUNCATE ((size_t)-1).
   C++ ONLY: this is an overload of the 5-arg form above. C has no overloading, so
   two _vsnprintf_s definitions made this header fail to compile for every .c source
   that force-includes it ("conflicting types for '_vsnprintf_s'"). All 4-arg callers
   are C++ (AtgUtil, FXLDumpInfo, TiledResources, ...); a .c source uses the 5-arg
   MSVC signature. */
#ifdef __cplusplus
static inline int _vsnprintf_s(char *d, size_t count, const char *fmt, va_list a)
{ return vsnprintf(d, count == (size_t)-1 ? 0x7fffffff : count, fmt, a); }
#endif
static inline int _snprintf_s(char *d, size_t dstsize, size_t count, const char *fmt, ...)
{ va_list a; int r; va_start(a, fmt); (void)count; r = vsnprintf(d, dstsize, fmt, a); va_end(a); return r; }
static inline int _snwprintf_s(wchar_t *d, size_t dstsize, size_t count, const wchar_t *fmt, ...)
{ va_list a; int r; va_start(a, fmt); (void)count; r = vswprintf(d, dstsize, fmt, a); va_end(a); return r; }

/* ---- lowercase in place ---- */
static inline int _wcslwr_s(wchar_t *s, size_t size)
{ size_t i; for (i = 0; i < size && s[i]; ++i) s[i] = (wchar_t)towlower(s[i]); return 0; }

/* ---- integer to ASCII (base 10 and 16 cover XDK use) ---- */
static inline int _itoa_s(int val, char *buf, size_t size, int radix)
{ if (radix == 16) return snprintf(buf, size, "%x", (unsigned)val) < 0 ? 22 : 0;
  return snprintf(buf, size, "%d", val) < 0 ? 22 : 0; }
static inline int _itow_s(int val, wchar_t *buf, size_t size, int radix)
{ if (radix == 16) return swprintf(buf, size, L"%x", (unsigned)val) < 0 ? 22 : 0;
  return swprintf(buf, size, L"%d", val) < 0 ? 22 : 0; }
static inline int _ltow_s(long val, wchar_t *buf, size_t size, int radix)
{ if (radix == 16) return swprintf(buf, size, L"%lx", (unsigned long)val) < 0 ? 22 : 0;
  return swprintf(buf, size, L"%ld", val) < 0 ? 22 : 0; }
static inline int _ultow_s(unsigned long val, wchar_t *buf, size_t size, int radix)
{ return swprintf(buf, size, radix == 16 ? L"%lx" : L"%lu", val) < 0 ? 22 : 0; }
static inline int _i64tow_s(long long val, wchar_t *buf, size_t size, int radix)
{ if (radix == 16) return swprintf(buf, size, L"%llx", (unsigned long long)val) < 0 ? 22 : 0;
  return swprintf(buf, size, L"%lld", val) < 0 ? 22 : 0; }
static inline int _ui64tow_s(unsigned long long val, wchar_t *buf, size_t size, int radix)
{ return swprintf(buf, size, radix == 16 ? L"%llx" : L"%llu", val) < 0 ? 22 : 0; }
/* _strcmpi is the old spelling of _stricmp. */
#ifndef _strcmpi
#define _strcmpi _stricmp
#endif

#ifdef __cplusplus
}
/* MSVC array-size-deducing overloads (C++ only). */
template <size_t N> inline int _wcslwr_s(wchar_t (&s)[N]) { return _wcslwr_s(s, N); }
template <size_t N> inline int _itoa_s(int val, char (&buf)[N], int radix) { return _itoa_s(val, buf, N, radix); }
template <size_t N> inline int _itow_s(int val, wchar_t (&buf)[N], int radix) { return _itow_s(val, buf, N, radix); }
template <size_t N> inline int _ltow_s(long val, wchar_t (&buf)[N], int radix) { return _ltow_s(val, buf, N, radix); }
template <size_t N> inline int _ultow_s(unsigned long val, wchar_t (&buf)[N], int radix) { return _ultow_s(val, buf, N, radix); }
template <size_t N> inline int _i64tow_s(long long val, wchar_t (&buf)[N], int radix) { return _i64tow_s(val, buf, N, radix); }
template <size_t N> inline int _ui64tow_s(unsigned long long val, wchar_t (&buf)[N], int radix) { return _ui64tow_s(val, buf, N, radix); }
/* _vsnprintf_s(buf, count, fmt, ap): buffer size deduced; count is usually
   _TRUNCATE (truncate to fit). Forward to vsnprintf bounded by the array size. */
template <size_t N> inline int _vsnprintf_s(char (&d)[N], size_t count, const char *fmt, va_list a)
{ (void)count; return vsnprintf(d, N, fmt, a); }
template <size_t N> inline int _vsnwprintf_s(wchar_t (&d)[N], size_t count, const wchar_t *fmt, va_list a)
{ (void)count; return vswprintf(d, N, fmt, a); }
/* _snprintf_s(buf, count, fmt, ...): buffer size deduced (the 3-arg secure form,
   distinct from the explicit-size overload above). */
template <size_t N> inline int _snprintf_s(char (&d)[N], size_t count, const char *fmt, ...)
{ va_list a; int r; va_start(a, fmt); (void)count; r = vsnprintf(d, N, fmt, a); va_end(a); return r; }
template <size_t N> inline int _snwprintf_s(wchar_t (&d)[N], size_t count, const wchar_t *fmt, ...)
{ va_list a; int r; va_start(a, fmt); (void)count; r = vswprintf(d, N, fmt, a); va_end(a); return r; }
#endif

/* ---- stack/heap alloc helpers. _malloca/_freea are paired; use the heap to
 *      avoid blowing the (small) title stack on large sizes. ---- */
#ifndef _alloca
#define _alloca(n) alloca(n)
#endif
#ifndef _malloca
#define _malloca(n) malloc(n)
#endif
#ifndef _freea
#define _freea(p) free(p)
#endif

#endif /* RXDK_MSVCRT_COMPAT_H */
