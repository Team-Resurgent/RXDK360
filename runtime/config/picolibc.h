/* Generated for RXDK-Libs Xbox HAL (picolibc 1.8.x profile). */
#pragma once

#define __ATOMIC_UNGETC 1
#define __FAST_STRCMP 1
#define __PICOLIBC_ERRNO_FUNCTION __rxdk_errno
#define __TINY_STDIO 1
#define __HAVE_COMPLEX 1
#define __IO_C99_FORMATS 1
#define __IO_LONG_LONG 1
/* %ls/%lc in the narrow printf family, which is what MSVC's %S/%C are rewritten
   to (libs/libc/xbox/ms_printf.c). Deliberately not __MB_CAPABLE: without it
   picolibc narrows a wchar_t by truncation rather than through a locale's
   multibyte encoding, which is the behaviour Xbox paths and gamertags want. */
#define __IO_WCHAR 1
/* Default printf/scanf I/O variant = double ('d'): float formatting/parsing for
   printf, scanf, and iostream num_put/num_get. Was 0 (no float -> picolibc
   emitted the "*float*" placeholder for %f/%g). */
#define __IO_DEFAULT 'd'
/* Exact binary <-> decimal conversion: tools/build_libc.py builds the Ryu
   sources (dtoa_ryu.c etc.), not the approximate *_engine.c ones. */
#define __IO_FLOAT_EXACT 1
/* printf produces every requested significant digit of a double exactly
   (dtoa_big.c), as glibc and MSVC do, rather than zero-filling past 17. */
#define __IO_FLOAT_ALL_DIGITS 1
#define __NEWLIB_VERSION "4.3.0"
#define _NEWLIB_VERSION "4.3.0"
#define _PICOLIBC_MINOR__ 8
#define _PICOLIBC_VERSION "1.8.11"
#define _PICOLIBC__ 1
#define __NEWLIB_MINOR__ 3
#define __NEWLIB_PATCHLEVEL__ 0
#define __NEWLIB__ 4
#define __PICOLIBC_MINOR__ 8
#define __PICOLIBC_PATCHLEVEL__ 11
#define __PICOLIBC_VERSION__ "1.8.11"
#define __PICOLIBC__ 1
#define ENABLE_PICOLIBC_EXIT 1
#define __HAVE_POSIX_LOCALE_API 1
#define __OBSOLETE_MATH_FLOAT 0
#define __OBSOLETE_MATH_DOUBLE 0
/* Legacy newlib ctype mask macros (_U/_L/_N/_S/_P/_C/_X/_B) are single-uppercase-
   letter object-like macros that collide with the stock XDK headers, which use those
   names as identifiers -- e.g. float.h's `double _chgsign(double _X)` and _X/_C
   throughout math.h -- in ANY translation unit that pulls <ctype.h> before an XDK
   header (Debug configs hit this readily). The 360 therefore does NOT opt into
   _PICOLIBC_LEGACY_CTYPE_MACROS at all.

   libc++ is the only consumer that needed them: <__locale_dir/ctype_base.h>'s
   _LIBCPP_LIBC_NEWLIB path built its ctype masks from _S/_P/_U/.... The installer's
   StageClang (PatchLibcxxCtype) rewrites that header to read picolibc's non-colliding
   __CTYPE_UPPER..__CTYPE_HEX instead (identical values -> ABI-neutral), so it no
   longer needs the legacy names. The original Xbox keeps them via ctype.h's __i386__
   arm, which is left intact. */
