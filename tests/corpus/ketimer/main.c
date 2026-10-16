#include "rt.h"
/* Kernel DPCs and timers: KeInsertQueueDpc / KeRemoveQueueDpc, KeSetTimerEx /
   KeCancelTimer, and waiting on a KTIMER. The same output is expected from a
   devkit and from xenia, so this pins xenia's emulation to the console's.

   - A DPC queued at PASSIVE_LEVEL runs before KeInsertQueueDpc returns, at
     DISPATCH_LEVEL, with the arguments it was queued with.
   - Queued at DISPATCH_LEVEL it waits for the IRQL to drop; queueing it again
     meanwhile returns FALSE, and KeRemoveQueueDpc takes it back off.
   - KeSetTimerEx and KeCancelTimer return whether the timer was set; a
     one-shot timer is no longer set once it has expired.
   - A periodic timer's DPC runs at DISPATCH_LEVEL with the system time as its
     arguments, and stops once the timer is cancelled. */

typedef struct { unsigned lo, hi; } large_t;
void KeInitializeDpc(void *dpc, void *routine, void *context);
int  KeInsertQueueDpc(void *dpc, void *arg1, void *arg2);
int  KeRemoveQueueDpc(void *dpc);
void KeInitializeTimerEx(void *timer, int type, unsigned process_type);
int  KeSetTimerEx(void *timer, long long due_time, long period, void *dpc);
int  KeCancelTimer(void *timer);
int  KeWaitForSingleObject(void *object, int reason, int mode, int alertable,
                           long long *timeout);
void KeQuerySystemTime(long long *time);
unsigned char KeGetCurrentProcessType(void);
unsigned char KeRaiseIrqlToDpcLevel(void);
void KfLowerIrql(unsigned char irql);
void Sleep(unsigned ms);

static unsigned char g_dpc[64] __attribute__((aligned(16)));
static unsigned char g_tdpc[64] __attribute__((aligned(16)));
static unsigned char g_timer[64] __attribute__((aligned(16)));
static unsigned char g_ptimer[64] __attribute__((aligned(16)));
static volatile int g_runs, g_truns;
static volatile unsigned g_irql, g_tirql, g_arg1, g_arg2, g_ctx;
static volatile void *g_self;
static volatile long long g_ttime;

static unsigned pcr_irql(void)
{
    unsigned irql;
    __asm__ volatile("lbz %0, 0x18(13)" : "=r"(irql));
    return irql;
}

static void dpc_routine(void *dpc, void *ctx, void *a1, void *a2)
{
    g_self = dpc;
    g_ctx = (unsigned)(unsigned long)ctx;
    g_arg1 = (unsigned)(unsigned long)a1;
    g_arg2 = (unsigned)(unsigned long)a2;
    g_irql = pcr_irql();
    g_runs++;
}

static void timer_dpc(void *dpc, void *ctx, void *a1, void *a2)
{
    (void)dpc; (void)ctx;
    g_ttime = ((long long)(unsigned long)a2 << 32) | (unsigned long)a1;
    g_tirql = pcr_irql();
    g_truns++;
}

static void dpcs(void)
{
    unsigned char old;
    int a, b, c, d, e, before, raised;

    KeInitializeDpc(g_dpc, (void *)dpc_routine, (void *)0xC0DE);
    a = KeInsertQueueDpc(g_dpc, (void *)11, (void *)22);
    DbgPrint("passive: insert %d, ran %d time(s) before it returned, irql %u, "
             "dpc %s, context %X, args %u %u\n", a, g_runs, g_irql,
             g_self == g_dpc ? "self" : "OTHER", g_ctx, g_arg1, g_arg2);

    before = g_runs;
    old = KeRaiseIrqlToDpcLevel();
    a = KeInsertQueueDpc(g_dpc, (void *)33, (void *)44);
    b = KeInsertQueueDpc(g_dpc, (void *)55, (void *)66);
    c = KeRemoveQueueDpc(g_dpc);
    d = KeRemoveQueueDpc(g_dpc);
    e = KeInsertQueueDpc(g_dpc, (void *)77, (void *)88);
    raised = g_runs - before;
    KfLowerIrql(old);
    /* Printed after lowering: a kit delivers DbgPrint made at DISPATCH_LEVEL
       after later output. */
    DbgPrint("dispatch: insert %d, again %d, remove %d, again %d, insert %d, "
             "ran %d time(s) while raised\n", a, b, c, d, e, raised);
    DbgPrint("lowered: ran %d time(s), args %u %u\n", g_runs - before, g_arg1, g_arg2);
}

static void one_shot(void)
{
    int a, b, w, c;

    KeInitializeTimerEx(g_timer, 0 /* NotificationTimer */, KeGetCurrentProcessType());
    a = KeSetTimerEx(g_timer, -100000LL, 0, 0);           /* 10 ms */
    b = KeSetTimerEx(g_timer, -100000LL, 0, 0);
    w = KeWaitForSingleObject(g_timer, 0, 0, 0, 0);
    c = KeCancelTimer(g_timer);
    DbgPrint("one-shot: set %d, set again %d, wait %X, cancel after expiry %d\n",
             a, b, w, c);
}

static void periodic(void)
{
    long long before, after;
    int a, c, runs, i;

    KeInitializeDpc(g_tdpc, (void *)timer_dpc, 0);
    KeInitializeTimerEx(g_ptimer, 1 /* SynchronizationTimer */, KeGetCurrentProcessType());
    KeQuerySystemTime(&before);
    a = KeSetTimerEx(g_ptimer, -10000LL, 2, g_tdpc);       /* 1 ms, every 2 ms */
    for (i = 0; i < 1000 && g_truns < 20; i++)
        Sleep(1);
    KeQuerySystemTime(&after);
    c = KeCancelTimer(g_ptimer);
    runs = g_truns;
    Sleep(20);
    DbgPrint("periodic: set %d, %s, irql %u, args %s, cancel %d, %s after cancel\n",
             a, runs >= 20 ? "20 DPCs" : "too few DPCs", g_tirql,
             g_ttime >= before && g_ttime <= after + 10000000LL ? "the system time" : "NOT the system time",
             c, g_truns - runs <= 1 ? "stopped" : "KEPT RUNNING");
}

void t_ketimer(void)
{
    dpcs();
    one_shot();
    periodic();
}
