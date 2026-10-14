/*
 * 2026 - Team Resurgent
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Part of RXDK - see LICENSE.md for the full GNU GPL v3.
 */

/*
 * Working-directory + path resolution shared by fileio.c and dirio.c.
 *
 * The Xbox 360 object manager has no per-process current directory: every path
 * handed to NtCreateFile must be fully qualified ("drive:\a\b"). To let portable
 * code use relative paths and chdir()/getcwd(), libc keeps its own cwd and
 * resolves every path through __rxdk_resolve_path():
 *   - '/' is translated to '\';
 *   - a path with no "drive:" is taken relative to the cwd;
 *   - '.' and '..' components (and duplicate separators) are collapsed.
 * Kernel calls take __rxdk_nt_path(), the same path under "\??\".
 *
 * The cwd defaults to "game:\" -- the drive a title is launched from (the same
 * mount xenia exposes the XEX's folder as, and the deployment root on hardware).
 */
#define _GNU_SOURCE 1
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

extern int stat(const char *path, struct stat *st);

static char g_cwd[PATH_MAX] = "game:\\";

const char *__rxdk_resolve_path(const char *in, char *out, size_t n) {
    char raw[PATH_MAX];
    size_t ri = 0, i;
    const char *colon, *s;
    char *o, *base;

    if (!in || n == 0)
        return in;

    /* raw = (relative ? cwd + '\\') + translated(in) */
    if (!strchr(in, ':')) {
        size_t c = strlen(g_cwd);
        for (i = 0; i < c && ri + 1 < sizeof raw; ++i)
            raw[ri++] = g_cwd[i];
        if (ri && raw[ri - 1] != '\\' && ri + 1 < sizeof raw)
            raw[ri++] = '\\';
    }
    for (i = 0; in[i] && ri + 1 < sizeof raw; ++i)
        raw[ri++] = (in[i] == '/') ? '\\' : in[i];
    raw[ri] = '\0';

    /* copy the "drive:" prefix verbatim, then normalise the components */
    o = out;
    colon = strchr(raw, ':');
    if (colon) {
        size_t plen = (size_t)(colon - raw) + 1;   /* include the ':' */
        if (plen > n - 1)
            plen = n - 1;
        memcpy(o, raw, plen);
        o += plen;
        s = colon + 1;
    } else {
        s = raw;
    }
    base = o;

    while (*s) {
        const char *e;
        size_t clen;
        while (*s == '\\')                       /* skip separators */
            ++s;
        if (!*s)
            break;
        for (e = s; *e && *e != '\\'; ++e)
            ;
        clen = (size_t)(e - s);
        if (clen == 1 && s[0] == '.') {
            /* current directory: drop */
        } else if (clen == 2 && s[0] == '.' && s[1] == '.') {
            if (o > base) {                      /* pop the last component */
                --o;
                while (o > base && o[-1] != '\\')
                    --o;
                if (o > base)
                    --o;                         /* also drop its leading '\' */
            }
        } else if ((size_t)(o - out) + clen + 2 < n) {
            *o++ = '\\';
            memcpy(o, s, clen);
            o += clen;
        }
        s = e;
    }
    if (o == base && (size_t)(o - out) + 2 < n)  /* drive root */
        *o++ = '\\';
    *o = '\0';
    return out;
}

/* The kernel object name for a path: resolved as above, then put under "\??\",
   where the console's drive links (game:, cache:, ...) live. NtCreateFile and
   FileRenameInformation take object names, and the console rejects a bare
   "game:\x" with STATUS_OBJECT_NAME_INVALID; "\??\game:\x" opens it (the XDK's
   CreateFile adds the same prefix). The cwd and getcwd() keep the drive form. */
const char *__rxdk_nt_path(const char *in, char *out, size_t n) {
    static const char prefix[] = "\\??\\";
    const size_t plen = sizeof prefix - 1;
    if (!in || n <= plen)
        return in;
    __rxdk_resolve_path(in, out + plen, n - plen);
    memcpy(out, prefix, plen);
    return out;
}

char *getcwd(char *buf, size_t size) {
    size_t n = strlen(g_cwd);
    if (!buf) {
        buf = (char *)malloc(n + 1);
        if (!buf) { errno = ENOMEM; return NULL; }
    } else if (size <= n) {
        errno = ERANGE;
        return NULL;
    }
    memcpy(buf, g_cwd, n + 1);
    return buf;
}

int chdir(const char *path) {
    char resolved[PATH_MAX];
    const char *colon;
    size_t nlen;
    __rxdk_resolve_path(path, resolved, sizeof resolved);
    nlen = strlen(resolved);

    /* A bare drive root ("xxx:\") always exists; otherwise the target must be a
       real directory. */
    colon = strchr(resolved, ':');
    if (!(colon && colon[1] == '\\' && colon[2] == '\0')) {
        struct stat st;
        if (stat(resolved, &st) != 0) { errno = ENOENT; return -1; }
        if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    }
    if (nlen + 1 >= sizeof g_cwd) { errno = ENAMETOOLONG; return -1; }
    memcpy(g_cwd, resolved, nlen + 1);
    return 0;
}

char *realpath(const char *path, char *resolved) {
    char tmp[PATH_MAX];
    size_t n;
    __rxdk_resolve_path(path, tmp, sizeof tmp);
    n = strlen(tmp);
    if (!resolved) {
        resolved = (char *)malloc(n + 1);
        if (!resolved) { errno = ENOMEM; return NULL; }
    }
    memcpy(resolved, tmp, n + 1);
    return resolved;
}
