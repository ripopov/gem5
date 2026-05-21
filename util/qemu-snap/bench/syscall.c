/*
 * syscall.c - Linux syscall / kernel exerciser for gem5 QEMU-snapshot mode.
 *
 * Where bench.c stresses the CPU pipeline and philo.c stresses SMP, this
 * testcase stresses the *kernel* and gem5's full-system plumbing: after the
 * snapshot is restored it drives a battery of system calls and checks each
 * one behaves correctly.  It validates that, on a restored snapshot, gem5
 * still correctly runs the parts of a system a pure compute loop never
 * touches:
 *
 *   - traps into S-mode and back (every syscall is an ecall),
 *   - page-fault handling and demand paging (mmap of fresh anonymous pages),
 *   - process creation and the SMP-capable scheduler (fork + waitpid),
 *   - the timer-interrupt path (nanosleep blocks until a timer tick),
 *   - signal delivery (a SIGUSR1 handler runs),
 *   - VFS / tmpfs I/O, pipes and poll().
 *
 * Flow the snapshot bridge relies on (same contract as bench.c / philo.c):
 *   1. snapshot_barrier() -- the race-free capture point.  qemu-snapshot.py
 *      breakpoints here, so QEMU halts at exactly this instruction and the
 *      whole syscall battery then runs under gem5 after the restore.
 *   2. run every check -- the region of interest, exercised in detail.
 *   3. report "SYSCALL-DONE ok pass=<n> total=<n>" on the console with
 *      write(2); m5_exit only ends the simulation cleanly afterwards.
 *
 * Determinism: every check has a fixed pass/fail outcome, so the pass count
 * is identical on every CPU model and memory system.
 */
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*
 * The explicit snapshot barrier.  Kept in its own non-inlined function so it
 * has a stable symbol address for qemu-snapshot.py to breakpoint on.
 */
__attribute__((noinline)) void
snapshot_barrier(void)
{
    __asm__ volatile("nop" ::: "memory");
}

/* gem5 m5ops: instruction = 0x0000007b | (func << 25); func 0x21 = m5_exit. */
static inline void
m5_exit(uint64_t delay)
{
    register uint64_t a0 __asm__("a0") = delay;
    __asm__ volatile(".word 0x4200007b" : : "r"(a0) : "memory");
}

/* Append an unsigned decimal number to p, return the new end pointer. */
static char *
u64_to_dec(char *p, uint64_t v)
{
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *p++ = tmp[--n];
    return p;
}

/* --------------------------------------------------------------------------
 * The checks.  Each returns 1 on success, 0 on failure, and prints a single
 * "  <name> .. ok/FAIL" line so a failing run is easy to diagnose.
 * ------------------------------------------------------------------------ */
static int
report(const char *name, int ok)
{
    char line[64];
    char *p = line;
    *p++ = ' ';
    *p++ = ' ';
    while (*name)
        *p++ = *name++;
    const char *tail = ok ? " .. ok\n" : " .. FAIL\n";
    while (*tail)
        *p++ = *tail++;
    write(1, line, (size_t)(p - line));
    return ok ? 1 : 0;
}

/* File I/O on tmpfs: open / write / lseek / read-back / fstat / close. */
static int
check_file_io(void)
{
    const char *path = "/tmp/syscall.dat";
    const char payload[] = "qemu-snap syscall testcase payload\n";
    int ok = 1;

    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0)
        return report("file_io", 0);
    if (write(fd, payload, sizeof(payload) - 1) != (ssize_t)sizeof(payload) - 1)
        ok = 0;
    if (lseek(fd, 0, SEEK_SET) != 0)
        ok = 0;

    char buf[64];
    ssize_t got = read(fd, buf, sizeof(buf));
    if (got != (ssize_t)sizeof(payload) - 1
        || memcmp(buf, payload, (size_t)got) != 0)
        ok = 0;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size != (off_t)sizeof(payload) - 1)
        ok = 0;
    close(fd);
    if (unlink(path) != 0)
        ok = 0;
    return report("file_io", ok);
}

/* Directory + working-directory syscalls. */
static int
check_dir(void)
{
    const char *dir = "/tmp/syscall.dir";
    int ok = 1;
    char cwd[128];

    if (getcwd(cwd, sizeof(cwd)) == NULL)
        ok = 0;
    if (mkdir(dir, 0755) != 0)
        ok = 0;
    if (chdir(dir) != 0)
        ok = 0;
    if (chdir("/") != 0)
        ok = 0;
    if (rmdir(dir) != 0)
        ok = 0;
    return report("dir", ok);
}

/* Anonymous mmap: fault in fresh pages, touch them, unmap. */
static int
check_mmap(void)
{
    const size_t len = 256 * 1024;
    int ok = 1;

    unsigned char *m = mmap(NULL, len, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED)
        return report("mmap", 0);
    /* Touch every page (demand-paged) and read it back. */
    for (size_t i = 0; i < len; i += 4096)
        m[i] = (unsigned char)(i >> 12);
    for (size_t i = 0; i < len; i += 4096)
        if (m[i] != (unsigned char)(i >> 12))
            ok = 0;
    if (munmap(m, len) != 0)
        ok = 0;
    return report("mmap", ok);
}

/* pipe(2): write into one end, read it back from the other. */
static int
check_pipe(void)
{
    int fds[2];
    int ok = 1;
    const char msg[] = "pipe-payload";
    char buf[32];

    if (pipe(fds) != 0)
        return report("pipe", 0);
    if (write(fds[1], msg, sizeof(msg)) != (ssize_t)sizeof(msg))
        ok = 0;

    /* poll() the read end: data is queued, so POLLIN must be set. */
    struct pollfd pfd = { .fd = fds[0], .events = POLLIN };
    if (poll(&pfd, 1, 1000) != 1 || !(pfd.revents & POLLIN))
        ok = 0;

    if (read(fds[0], buf, sizeof(buf)) != (ssize_t)sizeof(msg)
        || memcmp(buf, msg, sizeof(msg)) != 0)
        ok = 0;
    close(fds[0]);
    close(fds[1]);
    return report("pipe", ok);
}

/* fork(2) + waitpid(2): process creation and the scheduler. */
static int
check_fork(void)
{
    pid_t pid = fork();
    if (pid < 0)
        return report("fork", 0);
    if (pid == 0)
        _exit(42);                  /* child */

    int status = 0;
    int ok = 1;
    if (waitpid(pid, &status, 0) != pid)
        ok = 0;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 42)
        ok = 0;
    return report("fork", ok);
}

/* Timer-interrupt path: nanosleep must block, then time must have advanced. */
static int
check_time(void)
{
    struct timespec t0, t1;
    int ok = 1;

    if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0)
        ok = 0;
    struct timespec nap = { .tv_sec = 0, .tv_nsec = 2 * 1000 * 1000 };
    if (nanosleep(&nap, NULL) != 0)
        ok = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &t1) != 0)
        ok = 0;

    /* Monotonic clock must not go backwards, and must have advanced across
     * the sleep -- proof the timer interrupt fired and woke the process. */
    int64_t d = (int64_t)(t1.tv_sec - t0.tv_sec) * 1000000000
                + (t1.tv_nsec - t0.tv_nsec);
    if (d <= 0)
        ok = 0;
    return report("time", ok);
}

/* Signal delivery: a SIGUSR1 handler must run when the signal is raised. */
static volatile sig_atomic_t got_signal;

static void
on_sigusr1(int sig)
{
    (void)sig;
    got_signal = 1;
}

static int
check_signal(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigusr1;
    int ok = 1;

    if (sigaction(SIGUSR1, &sa, NULL) != 0)
        ok = 0;
    got_signal = 0;
    if (kill(getpid(), SIGUSR1) != 0)
        ok = 0;
    if (got_signal != 1)
        ok = 0;
    return report("signal", ok);
}

/* uname(2) + the trivial identity syscalls. */
static int
check_ids(void)
{
    struct utsname uts;
    int ok = 1;

    if (uname(&uts) != 0 || uts.sysname[0] == '\0')
        ok = 0;
    if (getpid() <= 0)
        ok = 0;
    if (getppid() < 0)
        ok = 0;
    return report("ids", ok);
}

int
main(void)
{
    write(1, "QEMU-SNAP-MODE-SYSCALL-READY\n", 28);

    /* (1) Race-free snapshot point: everything below runs under gem5. */
    snapshot_barrier();

    /* (2) The region of interest: drive every syscall check. */
    int (*const checks[])(void) = {
        check_ids, check_file_io, check_dir, check_mmap,
        check_pipe, check_fork, check_time, check_signal,
    };
    const int total = (int)(sizeof(checks) / sizeof(checks[0]));
    int passed = 0;
    for (int i = 0; i < total; i++)
        passed += checks[i]();

    /* (3) Report on the console - reaches gem5's terminal once PLIC/UART
     * state is restored and the UART interrupt path is live. */
    char msg[80];
    char *p = msg;
    const char *tag = (passed == total) ? "SYSCALL-DONE ok pass="
                                        : "SYSCALL-DONE BAD pass=";
    while (*tag)
        *p++ = *tag++;
    p = u64_to_dec(p, (uint64_t)passed);
    const char *t = " total=";
    while (*t)
        *p++ = *t++;
    p = u64_to_dec(p, (uint64_t)total);
    *p++ = '\n';
    write(1, msg, (size_t)(p - msg));

    /* Wait for the console output to drain through the (interrupt-driven)
     * UART, then end the gem5 simulation cleanly. */
    tcdrain(1);
    m5_exit(0);
    return 0;
}
