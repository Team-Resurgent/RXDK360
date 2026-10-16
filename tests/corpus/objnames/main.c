#include "rt.h"
/* Which object names NtCreateFile accepts, as status codes, so xenia can be
   held to the console's rules (it used to accept a bare "game:\" name, which
   hid a runtime bug that only showed on a devkit).

   Every name opens the game: directory (present on a kit and under xenia,
   whatever its writability) or a drive that does not exist. A relative name
   with no root directory is left out on purpose: on a devkit that once wedged
   the console until XBDM died (issue #4, tests/corpus/fileio). */

typedef struct { unsigned short Length, MaximumLength; char *Buffer; } ANSI_STRING;
typedef struct { long Status; unsigned long Information; } IO_STATUS_BLOCK;
typedef struct { void *RootDirectory; ANSI_STRING *ObjectName; unsigned long Attributes; } OBJECT_ATTRIBUTES;

void RtlInitAnsiString(ANSI_STRING *dst, const char *src);
long NtCreateFile(void **handle, unsigned long access, OBJECT_ATTRIBUTES *obja,
                  IO_STATUS_BLOCK *iosb, void *alloc, unsigned long attrs,
                  unsigned long share, unsigned long disp, unsigned long options);
long NtClose(void *handle);
long NtQueryFullAttributesFile(OBJECT_ATTRIBUTES *obja, void *info);

#define SYNCHRONIZE          0x00100000
#define FILE_LIST_DIRECTORY  0x00000001
#define OBJ_CASE_INSENSITIVE 0x00000040
#define FILE_SHARE_ALL       0x00000007
#define FILE_OPEN            0x00000001
#define FILE_DIRECTORY_FILE  0x00000001
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020

static long open_dir(void *root, const char *name, void **out)
{
    ANSI_STRING s;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    RtlInitAnsiString(&s, name);
    oa.RootDirectory = root;
    oa.ObjectName = &s;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    *out = 0;
    return NtCreateFile(out, SYNCHRONIZE | FILE_LIST_DIRECTORY, &oa, &iosb, 0, 0,
                        FILE_SHARE_ALL, FILE_OPEN,
                        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT);
}

static void query(const char *label, const char *name)
{
    ANSI_STRING s;
    OBJECT_ATTRIBUTES oa;
    unsigned char info[64];
    RtlInitAnsiString(&s, name);
    oa.RootDirectory = 0;
    oa.ObjectName = &s;
    oa.Attributes = OBJ_CASE_INSENSITIVE;
    DbgPrint("%-24s %08X\n", label, (unsigned)NtQueryFullAttributesFile(&oa, info));
}

static void probe(const char *label, void *root, const char *name)
{
    void *h;
    long st = open_dir(root, name, &h);
    DbgPrint("%-24s %08X\n", label, (unsigned)st);
    if (st >= 0 && h)
        NtClose(h);
}

void t_objnames(void)
{
    void *game;
    long st;

    probe("\\??\\game:\\", 0, "\\??\\game:\\");
    probe("game:\\ (bare)", 0, "game:\\");
    probe("\\??\\game:", 0, "\\??\\game:");
    probe("\\??\\nosuchdrive:\\", 0, "\\??\\nosuchdrive:\\");
    probe("game:\\ under \\??\\", (void *)0xFFFFFFFD, "game:\\");
    probe("missing file", 0, "\\??\\game:\\rxdk_absent_7c1d");
    probe("missing directory", 0, "\\??\\game:\\rxdk_absent_7c1d\\x");
    query("query \\??\\game:\\", "\\??\\game:\\");
    query("query game:\\ (bare)", "game:\\");
    query("query missing file", "\\??\\game:\\rxdk_absent_7c1d");

    st = open_dir(0, "\\??\\game:\\", &game);
    if (st >= 0) {
        probe("\"\" under game:\\", game, "");
        NtClose(game);
    } else {
        DbgPrint("could not open game:\\ as a root (%08X)\n", (unsigned)st);
    }
}
