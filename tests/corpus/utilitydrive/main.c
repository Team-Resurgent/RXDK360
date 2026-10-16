#include "rt.h"
/* XMountUtilityDrive, end to end, with the same output expected from a devkit
   and from xenia (cache-partitions).

   xapilib's XMountUtilityDrive reads the console's cache-partition table
   (1 KB at partition0 + 0x800, signed with the console's key), gives the
   title one of the two cache partitions (formatting it FATX if the title is
   new to it), and links \??\cache: to it. This checks each piece as a
   console leaves it:
   - the table: magic, version, this title in the current slot, and the
     signature (it verifies, a changed hash does not, signing again gives the
     same bytes);
   - the title's raw partition: a disk, 512-byte sectors, 2 GB, an XTAF
     superblock with the cluster size asked for;
   - the volume on it: FATX, its geometry, its serial the superblock's;
   - cache:\ itself: a file written there reads back. */

typedef struct { unsigned short Length, MaximumLength; char *Buffer; } ANSI_STRING;
typedef struct { long Status; unsigned long Information; } IO_STATUS_BLOCK;
typedef struct { void *RootDirectory; ANSI_STRING *ObjectName; unsigned long Attributes; } OBJECT_ATTRIBUTES;

unsigned long XMountUtilityDrive(int format_clean, unsigned long bytes_per_cluster,
                                 unsigned long file_cache_size);
void RtlInitAnsiString(ANSI_STRING *dst, const char *src);
long NtCreateFile(void **h, unsigned long access, OBJECT_ATTRIBUTES *oa, IO_STATUS_BLOCK *iosb,
                  void *alloc, unsigned long attrs, unsigned long share, unsigned long disp,
                  unsigned long options);
long NtReadFile(void *h, void *ev, void *apc, void *ctx, IO_STATUS_BLOCK *iosb, void *buf,
                unsigned long len, long long *offset);
long NtWriteFile(void *h, void *ev, void *apc, void *ctx, IO_STATUS_BLOCK *iosb, const void *buf,
                 unsigned long len, long long *offset);
long NtQueryVolumeInformationFile(void *h, IO_STATUS_BLOCK *iosb, void *buf, unsigned long len,
                                  unsigned long cls);
long NtDeviceIoControlFile(void *h, void *ev, void *apc, void *ctx, IO_STATUS_BLOCK *iosb,
                           unsigned long code, void *in, unsigned long inlen, void *out,
                           unsigned long outlen);
long NtClose(void *h);
void XeCryptSha(const void *a, unsigned long alen, const void *b, unsigned long blen,
                const void *c, unsigned long clen, void *out, unsigned long outlen);
int  XeKeysConsoleSignatureVerification(const void *hash, const void *sig, int *cert_valid);
int  XeKeysConsolePrivateKeySign(const void *hash, void *sig);
unsigned XapiGetCurrentTitleId(void);

void *memset(void *d, int c, unsigned int n);

#define CLUSTER 0x4000

static unsigned be32(const unsigned char *p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}

/* access, disposition and options as given; share read/write/delete */
static long nt_open(const char *name, unsigned long access, unsigned long disp,
                    unsigned long options, void **h)
{
    ANSI_STRING s;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    RtlInitAnsiString(&s, name);
    oa.RootDirectory = 0;
    oa.ObjectName = &s;
    oa.Attributes = 0x40;
    return NtCreateFile(h, access | 0x00100000 /* SYNCHRONIZE */, &oa, &iosb, 0, 0x80, 7,
                        disp, options | 0x20 /* FILE_SYNCHRONOUS_IO_NONALERT */);
}

static unsigned g_title_id;

static int table(void)
{
    static unsigned char t[1024], sig[0x228];
    unsigned char hash[20];
    IO_STATUS_BLOCK iosb;
    long long off = 0x800;
    void *h;
    int slot, ok, bad, cert, cert2, fresh, same;
    long st = nt_open("\\Device\\Harddisk0\\partition0", 0x1 /* FILE_READ_DATA */, 1, 0, &h);
    if (st < 0) {
        DbgPrint("table: open %08X\n", (unsigned)st);
        return -1;
    }
    st = NtReadFile(h, 0, 0, 0, &iosb, t, sizeof t, &off);
    NtClose(h);
    if (st < 0) {
        DbgPrint("table: read %08X\n", (unsigned)st);
        return -1;
    }
    slot = (int)be32(t + 632);
    XeCryptSha(t + 556, 88, 0, 0, 0, 0, hash, 20);
    ok = XeKeysConsoleSignatureVerification(hash, t + 4, &cert);
    hash[0] ^= 1;
    bad = XeKeysConsoleSignatureVerification(hash, t + 4, &cert2);
    hash[0] ^= 1;
    XeKeysConsolePrivateKeySign(hash, sig);
    same = memcmp(sig, t + 4, sizeof sig) == 0;
    fresh = XeKeysConsoleSignatureVerification(hash, sig, &cert2);
    DbgPrint("table: magic %s, version %u, this title in the current slot: %s\n",
             be32(t) == 0x4A6F7368 ? "Josh" : "WRONG", be32(t + 628),
             slot >= 0 && slot < 2 && be32(t + 636 + slot * 4) == g_title_id ? "yes" : "NO");
    DbgPrint("table: signature verifies %d (certificate %d), changed hash %d, "
             "signing again %s and verifies %d\n", ok, cert, bad,
             same ? "gives the same bytes" : "DIFFERS", fresh);
    return slot;
}

static unsigned raw(int slot)
{
    char name[32];
    void *h;
    IO_STATUS_BLOCK iosb;
    unsigned dev[2] = {0, 0}, geo[2] = {0, 0};
    unsigned long long part[2] = {0, 0};
    unsigned char sb[512];
    long long off = 0;
    long st;
    sprintf(name, "\\Device\\Harddisk0\\Cache%d", slot);
    st = nt_open(name, 0x1, 1, 0, &h);
    if (st < 0) {
        DbgPrint("partition: open %08X\n", (unsigned)st);
        return 0;
    }
    NtQueryVolumeInformationFile(h, &iosb, dev, 8, 4 /* FileFsDeviceInformation */);
    NtDeviceIoControlFile(h, 0, 0, 0, &iosb, 0x70000 /* drive geometry */, 0, 0, geo, 8);
    NtDeviceIoControlFile(h, 0, 0, 0, &iosb, 0x74004 /* partition info */, 0, 0, part, 16);
    st = NtReadFile(h, 0, 0, 0, &iosb, sb, sizeof sb, &off);
    NtClose(h);
    DbgPrint("partition: device type %u, %u-byte sectors, %X%08X bytes, superblock %s, "
             "%X sectors a cluster\n", dev[0], geo[1], (unsigned)(part[1] >> 32),
             (unsigned)part[1], st >= 0 && be32(sb) == 0x58544146 ? "XTAF" : "MISSING",
             be32(sb + 8));
    return be32(sb + 4);   /* the volume serial */
}

static void volume(int slot, unsigned serial)
{
    char name[32];
    void *h;
    IO_STATUS_BLOCK iosb;
    unsigned size[6] = {0}, vol[8] = {0}, attr[16] = {0};
    long st;
    sprintf(name, "\\Device\\Harddisk0\\Cache%d\\", slot);
    st = nt_open(name, 0x1, 1, 0x1 /* FILE_DIRECTORY_FILE */, &h);
    if (st < 0) {
        DbgPrint("volume: open %08X\n", (unsigned)st);
        return;
    }
    NtQueryVolumeInformationFile(h, &iosb, size, 24, 3 /* FileFsSizeInformation */);
    NtQueryVolumeInformationFile(h, &iosb, vol, 32, 1 /* FileFsVolumeInformation */);
    NtQueryVolumeInformationFile(h, &iosb, attr, 64, 5 /* FileFsAttributeInformation */);
    NtClose(h);
    DbgPrint("volume: %.4s, names up to %u, %u-byte clusters, %X%08X clusters, "
             "serial %s the superblock's\n", (char *)&attr[3], attr[1], size[4] * size[5],
             size[0], size[1], vol[2] == serial ? "is" : "is NOT");
}

static void cache_file(void)
{
    static const char text[] = "rxdk utility drive";
    char back[sizeof text];
    void *h;
    IO_STATUS_BLOCK iosb;
    long long off = 0;
    long w, r;
    w = nt_open("\\??\\cache:\\rxdk_utility.txt", 0x2 /* FILE_WRITE_DATA */,
                5 /* FILE_OVERWRITE_IF */, 0x40 /* FILE_NON_DIRECTORY_FILE */, &h);
    if (w >= 0) {
        w = NtWriteFile(h, 0, 0, 0, &iosb, text, sizeof text, &off);
        NtClose(h);
    }
    memset(back, 0, sizeof back);
    r = nt_open("\\??\\cache:\\rxdk_utility.txt", 0x1, 1, 0x40, &h);
    if (r >= 0) {
        off = 0;
        r = NtReadFile(h, 0, 0, 0, &iosb, back, sizeof back, &off);
        NtClose(h);
    }
    DbgPrint("cache: write %08X, read back %s\n", (unsigned)w,
             r >= 0 && memcmp(back, text, sizeof text) == 0 ? "the same" : "DIFFERENT");
}

void t_utilitydrive(void)
{
    unsigned long err;
    int slot;

    g_title_id = XapiGetCurrentTitleId();
    err = XMountUtilityDrive(0, CLUSTER, 0x10000);
    DbgPrint("XMountUtilityDrive: %lu\n", err);
    slot = table();
    if (slot >= 0 && slot < 2) {
        unsigned serial = raw(slot);
        volume(slot, serial);
    }
    cache_file();
}
