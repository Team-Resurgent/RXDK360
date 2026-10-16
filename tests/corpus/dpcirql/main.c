#include "rt.h"
/* Is libc's memcpy safe in a DPC, and where is the IRQL?

   The kernel forbids floating-point and vector use at DPC time. On a devkit
   (2026-10-15), a DPC that called the vectorized picolibc memcpy bugchecked
   the console with ATTEMPTED_FPU_USE_FROM_DPC (0xF0, from the kernel crash
   dump), while one copying with integer loads and stores ran.
   memcpy / memmove / mempcpy (runtime/xbox/memcopy_irql.c) now copy without
   VMX or FP above PASSIVE_LEVEL, reading the IRQL from 0x18(r13).

   The main thread holds a pattern in v0-v7 and f0-f7 and spins, bumping a
   progress counter, until a 1 ms periodic kernel timer has run its DPC 300
   times. The DPC copies, then waits ~20 us: if the progress counter did not
   move, the DPC interrupted the main thread on its own hardware thread (a DPC
   on another one would prove nothing). Hardware only: xenia runs a timer's
   DPC on a host thread of its own, so it never interrupts the main thread
   (tests/corpus/ketimer covers the timer and DPC semantics on both). */

void KeInitializeDpc(void *dpc, void *routine, void *context);
void KeInitializeTimerEx(void *timer, int type, unsigned process_type);
int  KeSetTimerEx(void *timer, long long due_time, long period, void *dpc);
int  KeCancelTimer(void *timer);
unsigned char KeGetCurrentProcessType(void);
unsigned char KeRaiseIrqlToDpcLevel(void);
void KfLowerIrql(unsigned char irql);

static unsigned char g_dpc[64] __attribute__((aligned(16)));
static unsigned char g_timer[64] __attribute__((aligned(16)));
static volatile int g_dpc_count, g_progress, g_interrupted, g_mode;
static volatile unsigned g_dpc_irql;
static volatile unsigned g_size;
static unsigned char g_src[4096] __attribute__((aligned(16)));
static unsigned char g_dst[4096] __attribute__((aligned(16)));

static unsigned pcr_irql(void)
{
    unsigned irql;
    __asm__ volatile("lbz %0, 0x18(13)" : "=r"(irql));
    return irql;
}

static unsigned long long timebase(void)
{
    unsigned long long tb;
    __asm__ volatile("mftb %0" : "=r"(tb));
    return tb;
}

static void dpc_routine(void *dpc, void *ctx, void *a1, void *a2)
{
    (void)dpc; (void)ctx; (void)a1; (void)a2;
    g_dpc_irql = pcr_irql();
    if (g_mode == 0) {
        unsigned i;
        for (i = 0; i < 4096; i += 4)          /* volatile word copy: no FP/VMX */
            *(volatile unsigned *)(g_dst + i) = *(volatile unsigned *)(g_src + i);
    } else {
        memcpy(g_dst, g_src, g_size);         /* volatile size: the libc call */
    }
    {
        int before = g_progress;
        unsigned long long t0 = timebase();
        while (timebase() - t0 < 1000)        /* 50 MHz timebase: 20 us */
            ;
        if (g_progress == before)
            g_interrupted++;
    }
    g_dpc_count++;
}

static void hold_regs(const unsigned *vin, unsigned *vout, const double *fin, double *fout, int fires)
{
    int target = g_dpc_count + fires;
    unsigned long spins = 0x7FFFFFFF;
    __asm__ volatile(
        "li 9, 0\n\t   lvx 0, %[vin], 9\n\t"
        "li 9, 16\n\t  lvx 1, %[vin], 9\n\t"
        "li 9, 32\n\t  lvx 2, %[vin], 9\n\t"
        "li 9, 48\n\t  lvx 3, %[vin], 9\n\t"
        "li 9, 64\n\t  lvx 4, %[vin], 9\n\t"
        "li 9, 80\n\t  lvx 5, %[vin], 9\n\t"
        "li 9, 96\n\t  lvx 6, %[vin], 9\n\t"
        "li 9, 112\n\t lvx 7, %[vin], 9\n\t"
        "lfd 0, 0(%[fin])\n\t  lfd 1, 8(%[fin])\n\t  lfd 2, 16(%[fin])\n\t lfd 3, 24(%[fin])\n\t"
        "lfd 4, 32(%[fin])\n\t lfd 5, 40(%[fin])\n\t lfd 6, 48(%[fin])\n\t lfd 7, 56(%[fin])\n\t"
        "1:\n\t"
        "lwz 10, 0(%[cnt])\n\t"
        "cmpw 10, %[target]\n\t"
        "bge 2f\n\t"
        "lwz 10, 0(%[prog])\n\t"
        "addi 10, 10, 1\n\t"
        "stw 10, 0(%[prog])\n\t"
        "addi %[spins], %[spins], -1\n\t"
        "cmpwi %[spins], 0\n\t"
        "bne 1b\n\t"
        "2:\n\t"
        "li 9, 0\n\t   stvx 0, %[vout], 9\n\t"
        "li 9, 16\n\t  stvx 1, %[vout], 9\n\t"
        "li 9, 32\n\t  stvx 2, %[vout], 9\n\t"
        "li 9, 48\n\t  stvx 3, %[vout], 9\n\t"
        "li 9, 64\n\t  stvx 4, %[vout], 9\n\t"
        "li 9, 80\n\t  stvx 5, %[vout], 9\n\t"
        "li 9, 96\n\t  stvx 6, %[vout], 9\n\t"
        "li 9, 112\n\t stvx 7, %[vout], 9\n\t"
        "stfd 0, 0(%[fout])\n\t  stfd 1, 8(%[fout])\n\t  stfd 2, 16(%[fout])\n\t stfd 3, 24(%[fout])\n\t"
        "stfd 4, 32(%[fout])\n\t stfd 5, 40(%[fout])\n\t stfd 6, 48(%[fout])\n\t stfd 7, 56(%[fout])\n\t"
        : [spins] "+r"(spins)
        : [vin] "b"(vin), [vout] "b"(vout), [fin] "b"(fin), [fout] "b"(fout),
          [cnt] "b"(&g_dpc_count), [prog] "b"(&g_progress), [target] "r"(target)
        : "r9", "r10", "cr0", "memory",
          "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
          "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7");
}

static void run(const char *name, int mode, unsigned size)
{
    static unsigned vpat[32] __attribute__((aligned(16))), vgot[32] __attribute__((aligned(16)));
    static double fpat[8], fgot[8];
    int bad = 0, before, ibefore, i;

    for (i = 0; i < 32; i++) { vpat[i] = 0x5EED0000u + (unsigned)i * 0x01010101u; vgot[i] = 0; }
    for (i = 0; i < 8; i++) { fpat[i] = 1000.25 + i; fgot[i] = 0; }
    for (i = 0; i < 4096; i++) { g_src[i] = (unsigned char)(i * 7 + 3); g_dst[i] = 0; }
    g_mode = mode;
    g_size = size;
    before = g_dpc_count;
    ibefore = g_interrupted;
    KeSetTimerEx(g_timer, -10000LL, 1, g_dpc);          /* 1 ms, then every 1 ms */
    hold_regs(vpat, vgot, fpat, fgot, 300);
    KeCancelTimer(g_timer);
    for (i = 0; i < 32; i++) if (vgot[i] != vpat[i]) bad++;
    for (i = 0; i < 8; i++) if (fgot[i] != fpat[i]) bad++;
    DbgPrint("%s: %s, %s the main thread, %s, copy %s\n", name,
             g_dpc_count - before >= 300 ? "300 DPCs" : "too few DPCs",
             g_interrupted - ibefore > 0 ? "interrupted" : "did not interrupt",
             bad ? "registers changed" : "v0-v7 and f0-f7 intact",
             memcmp(g_dst, g_src, mode ? size : 4096) == 0 ? "correct" : "wrong");
}

void t_dpcirql(void)
{
    unsigned char old;
    unsigned raised;

    DbgPrint("irql at PASSIVE_LEVEL: %u\n", pcr_irql());
    old = KeRaiseIrqlToDpcLevel();
    raised = pcr_irql();
    KfLowerIrql(old);
    DbgPrint("irql after KeRaiseIrqlToDpcLevel: %u\n", raised);

    KeInitializeDpc(g_dpc, (void *)dpc_routine, 0);
    KeInitializeTimerEx(g_timer, 1 /* SynchronizationTimer */, KeGetCurrentProcessType());
    run("integer copy in a DPC", 0, 0);
    DbgPrint("irql in a DPC: %u\n", g_dpc_irql);
    run("memcpy 256 in a DPC", 1, 256);
    run("memcpy 4096 in a DPC", 1, 4096);
}
