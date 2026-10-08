/*
 * 2026 - Team Resurgent
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Part of RXDK - see LICENSE.md for the full GNU GPL v3.
 */

/*
 * POSIX functions the console has no concept of -- processes, signals, virtual-
 * memory mapping, terminals, per-file permissions. They exist so portable POSIX
 * code links and runs; each takes the most compatible action: a fixed value
 * where there is a sensible one (getpid == 1, the single title; permission
 * changes succeed as no-ops on the permission-less FATX/GDFX volumes; mprotect
 * succeeds since title memory is already accessible), otherwise -1/ENOSYS.
 */
#define _GNU_SOURCE 1
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <termios.h>
#include <stdint.h>

/* ---- identity: one title, no users -------------------------------------- */
pid_t getpid(void)  { return 1; }
pid_t getppid(void) { return 0; }
uid_t getuid(void)  { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void)  { return 0; }
gid_t getegid(void) { return 0; }

/* ---- no process model --------------------------------------------------- */
pid_t fork(void)                             { errno = ENOSYS; return -1; }
pid_t waitpid(pid_t pid, int *st, int opts)  { (void)pid; (void)st; (void)opts; errno = ECHILD; return -1; }
int   pause(void)                            { errno = ENOSYS; return -1; } /* nothing to wake us */
/* kill/alarm are real (cooperative) in signals.c. */

extern void HalReturnToFirmware(unsigned int);
_Noreturn void _exit(int status) {
    (void)status;
    HalReturnToFirmware(2); /* reboot, as exit() does */
    for (;;) {}
}

/* ---- signals: signal/sigaction/raise/kill are a cooperative facility in
   signals.c (handlers run synchronously; no async pre-emption). The sigset_t
   helpers are picolibc inline macros. Only mask/block state is a no-op here --
   nothing is ever blocked because nothing is asynchronously delivered. -------- */
int sigprocmask(int how, const sigset_t *set, sigset_t *old) {
    (void)how; (void)set;
    if (old) *old = 0;               /* nothing is ever blocked */
    return 0;
}

/* ---- virtual memory: no mapping API. mprotect succeeds (title memory is
   already RWX to the guest); mmap/munmap report unsupported. sbrk is absent --
   the allocator uses the console pool, not a break. ------------------------- */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    (void)addr; (void)len; (void)prot; (void)flags; (void)fd; (void)off;
    errno = ENOSYS;
    return MAP_FAILED;
}
int   munmap(void *addr, size_t len)              { (void)addr; (void)len; errno = ENOSYS; return -1; }
int   mprotect(void *addr, size_t len, int prot)  { (void)addr; (void)len; (void)prot; return 0; }
void *sbrk(intptr_t incr)                         { (void)incr; errno = ENOMEM; return (void *)-1; }

/* ---- terminals: the console debug channel is not a POSIX tty ------------- */
int tcgetattr(int fd, struct termios *t)               { (void)fd; (void)t; errno = ENOTTY; return -1; }
int tcsetattr(int fd, int act, const struct termios *t){ (void)fd; (void)act; (void)t; errno = ENOTTY; return -1; }

/* ---- interval timers: setitimer/getitimer(ITIMER_REAL) are real in signals.c
   (a helper thread fires the SIGALRM handler). ---------------------------- */

/* ---- permissions: FATX/GDFX have none, so changes "succeed" as no-ops --- */
int    chmod(const char *path, mode_t mode)             { (void)path; (void)mode; return 0; }
int    chown(const char *path, uid_t owner, gid_t grp)  { (void)path; (void)owner; (void)grp; return 0; }
mode_t umask(mode_t mask)                               { (void)mask; return 0; }
