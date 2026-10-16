#include "rt.h"
/* Does data below the stack pointer survive on the console?

   clang keeps up to 288 bytes below r1 without a frame (the PPC64 red zone:
   a leaf function's locals, and register saves made before the stwu that
   allocates the frame). The XDK compiler goes further: a frameless leaf keeps
   up to 592 bytes there (148 ints at r1-592; at 149 it allocates a frame).
   Neither is safe unless an interrupt, DPC, context switch or kernel trap
   arriving mid-function leaves that area alone.

   rz_hold, a frameless leaf in assembly, writes a pattern to the 592 bytes
   below r1, optionally raises the debug-print trap (twi 31, 0, 20: a program
   exception the kernel handles and resumes from), spins until *count reaches
   target while bumping *progress, then counts the words that changed:
   - DPCs: a 1 ms periodic kernel timer runs its DPC 300 times; the DPC checks
     that *progress stood still while it ran, so it interrupted this thread on
     its own hardware thread.
   - Preemption: a busy thread of the same priority on the same hardware
     thread (checked with GetCurrentProcessorNumber) makes progress, which it
     can only do while this thread is switched out.
   Hardware only: xenia neither implements KeSetTimerEx nor runs interrupts,
   DPCs or other threads on a guest thread's stack. */

void KeInitializeDpc(void *dpc, void *routine, void *context);
void KeInitializeTimerEx(void *timer, int type, unsigned process_type);
int  KeSetTimerEx(void *timer, long long due_time, long period, void *dpc);
int  KeCancelTimer(void *timer);
unsigned char KeGetCurrentProcessType(void);
void *CreateThread(void *sa, unsigned stack, unsigned (*start)(void *), void *arg,
                   unsigned flags, unsigned *tid);
unsigned ResumeThread(void *thread);
unsigned XSetThreadProcessor(void *thread, unsigned processor);
unsigned GetCurrentProcessorNumber(void);
unsigned WaitForSingleObject(void *handle, unsigned ms);
int  CloseHandle(void *handle);

#define CREATE_SUSPENDED 4
#define RZ_BYTES 592
#define RZ_REACHED 0x10000u

/* unsigned rz_hold(volatile int *count, int target, volatile int *progress,
                    const char *msg, unsigned msglen)
   Returns the number of the RZ_BYTES/4 words below r1 that changed, plus
   RZ_REACHED if *count reached target while holding. Gives up spinning after
   ~2^31 iterations so a missing event cannot hang the test. */
unsigned rz_hold(volatile int *count, int target, volatile int *progress,
                 const char *msg, unsigned msglen);
__asm__(
    "  .text\n"
    "  .globl rz_hold\n"
    "  .p2align 2\n"
    "rz_hold:\n"
    "  addi  8, 1, -592\n"           /* r8: start of the area below r1 */
    "  lis   9, 0x5EED\n"
    "  ori   9, 9, 0x1234\n"         /* r9: pattern seed */
    "  li    10, 148\n"
    "  mtctr 10\n"
    "  mr    11, 8\n"
    "  mr    12, 9\n"
    "1:\n"
    "  stw   12, 0(11)\n"
    "  addi  11, 11, 4\n"
    "  addi  12, 12, 0x101\n"
    "  bdnz  1b\n"
    "  mr    11, 3\n"                /* r11: count (r3/r4 carry the trap's text) */
    "  mr    10, 4\n"                /* r10: target */
    "  cmpwi 6, 0\n"
    "  beq   2f\n"
    "  mr    3, 6\n"
    "  mr    4, 7\n"
    "  twi   31, 0, 20\n"            /* debug print: r3 = text, r4 = length */
    "2:\n"
    "  li    7, 0\n"                 /* r7: reached the target */
    "  lis   12, 0x7FFF\n"           /* r12: spin limit */
    "3:\n"
    "  lwz   0, 0(11)\n"
    "  cmpw  0, 10\n"
    "  bge   7f\n"
    "  lwz   6, 0(5)\n"              /* not r0: addi reads r0 as zero */
    "  addi  6, 6, 1\n"
    "  stw   6, 0(5)\n"
    "  addi  12, 12, -1\n"
    "  cmpwi 12, 0\n"
    "  bne   3b\n"
    "  b     4f\n"
    "7:\n"
    "  lis   7, 1\n"                 /* RZ_REACHED */
    "4:\n"
    "  li    3, 0\n"
    "  li    10, 148\n"
    "  mtctr 10\n"
    "  mr    11, 8\n"
    "  mr    12, 9\n"
    "5:\n"
    "  lwz   0, 0(11)\n"
    "  cmpw  0, 12\n"
    "  beq   6f\n"
    "  addi  3, 3, 1\n"
    "6:\n"
    "  addi  11, 11, 4\n"
    "  addi  12, 12, 0x101\n"
    "  bdnz  5b\n"
    "  or    3, 3, 7\n"
    "  blr\n");

/* The control: rz_fill and rz_check write and check the same pattern, so a
   call made between them from one caller (same r1) must show up as changed. */
void rz_fill(void);
unsigned rz_check(void);
__asm__(
    "  .text\n"
    "  .globl rz_fill\n"
    "  .p2align 2\n"
    "rz_fill:\n"
    "  addi  11, 1, -592\n"
    "  lis   12, 0x5EED\n"
    "  ori   12, 12, 0x1234\n"
    "  li    10, 148\n"
    "  mtctr 10\n"
    "1:\n"
    "  stw   12, 0(11)\n"
    "  addi  11, 11, 4\n"
    "  addi  12, 12, 0x101\n"
    "  bdnz  1b\n"
    "  blr\n"
    "  .globl rz_check\n"
    "  .p2align 2\n"
    "rz_check:\n"
    "  addi  11, 1, -592\n"
    "  lis   12, 0x5EED\n"
    "  ori   12, 12, 0x1234\n"
    "  li    10, 148\n"
    "  mtctr 10\n"
    "  li    3, 0\n"
    "1:\n"
    "  lwz   0, 0(11)\n"
    "  cmpw  0, 12\n"
    "  beq   2f\n"
    "  addi  3, 3, 1\n"
    "2:\n"
    "  addi  11, 11, 4\n"
    "  addi  12, 12, 0x101\n"
    "  bdnz  1b\n"
    "  blr\n");

static void __attribute__((noinline)) control(void)
{
    unsigned quiet, clobbered;
    rz_fill();
    quiet = rz_check();
    rz_fill();
    DbgPrint("control: calling DbgPrint between the fill and the check\n");
    clobbered = rz_check();
    DbgPrint("control: %s with nothing between, %s across a call\n",
             quiet ? "CHANGED" : "intact", clobbered ? "changed" : "NOT CHANGED");
}

static unsigned char g_dpc[64] __attribute__((aligned(16)));
static unsigned char g_timer[64] __attribute__((aligned(16)));
static volatile int g_dpc_count, g_progress, g_interrupted;
static volatile int g_other, g_stop;
static volatile unsigned g_other_proc;

static unsigned long long timebase(void)
{
    unsigned long long tb;
    __asm__ volatile("mftb %0" : "=r"(tb));
    return tb;
}

static void dpc_routine(void *dpc, void *ctx, void *a1, void *a2)
{
    int before = g_progress;
    unsigned long long t0 = timebase();
    (void)dpc; (void)ctx; (void)a1; (void)a2;
    while (timebase() - t0 < 1000)            /* 50 MHz timebase: 20 us */
        ;
    if (g_progress == before)
        g_interrupted++;
    g_dpc_count++;
}

static unsigned competitor(void *arg)
{
    (void)arg;
    g_other_proc = GetCurrentProcessorNumber();
    while (!g_stop)
        g_other++;
    return 0;
}

static void dpcs(const char *name, const char *msg)
{
    int before = g_dpc_count, ibefore = g_interrupted;
    unsigned r, changed;

    KeSetTimerEx(g_timer, -10000LL, 1, g_dpc);          /* 1 ms, then every 1 ms */
    r = rz_hold(&g_dpc_count, before + 300, &g_progress, msg, msg ? strlen(msg) : 0);
    KeCancelTimer(g_timer);
    changed = r & ~RZ_REACHED;
    DbgPrint("%s: %s, %s the holding thread, %d bytes below r1 %s\n", name,
             r & RZ_REACHED ? "300 DPCs" : "too few DPCs",
             g_interrupted - ibefore > 0 ? "interrupted" : "did not interrupt",
             RZ_BYTES, changed ? "CHANGED" : "intact");
    if (changed)
        DbgPrint("  %u of %d words changed\n", changed, RZ_BYTES / 4);
}

static void preemption(void)
{
    unsigned me = GetCurrentProcessorNumber(), r, changed;
    void *h;
    int start;

    g_stop = 0;
    g_other = 0;
    g_other_proc = 0xFFFFFFFFu;
    h = CreateThread(0, 0, competitor, 0, CREATE_SUSPENDED, 0);
    XSetThreadProcessor(h, me);
    ResumeThread(h);
    /* Hold until the competitor has counted 100000 on this hardware thread:
       it can only do that while the holding thread is switched out. */
    start = g_other;
    r = rz_hold(&g_other, start + 100000, &g_progress, 0, 0);
    changed = r & ~RZ_REACHED;
    g_stop = 1;
    WaitForSingleObject(h, 0xFFFFFFFFu);
    CloseHandle(h);
    DbgPrint("preemption: competitor %s, %s, %d bytes below r1 %s\n",
             g_other_proc == me ? "on the same hardware thread" : "on ANOTHER hardware thread",
             r & RZ_REACHED ? "ran while the holder was switched out" : "never ran",
             RZ_BYTES, changed ? "CHANGED" : "intact");
    if (changed)
        DbgPrint("  %u of %d words changed\n", changed, RZ_BYTES / 4);
}

void t_redzone(void)
{
    control();
    KeInitializeDpc(g_dpc, (void *)dpc_routine, 0);
    KeInitializeTimerEx(g_timer, 1 /* SynchronizationTimer */, KeGetCurrentProcessType());
    dpcs("DPCs", 0);
    dpcs("DPCs after a debug-print trap", "(redzone: debug-print trap from a frameless leaf)\n");
    preemption();
}
