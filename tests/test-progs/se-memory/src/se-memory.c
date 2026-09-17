/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * A syscall-emulation workload for CPU models that read and write guest
 * memory without going through the memory system, such as
 * DirectMemorySimpleCPU.  Those models translate an address and then touch
 * the backing store directly, so the cases they can get wrong are the ones
 * where the mapping changes underneath them or where somebody other than
 * the CPU writes the same bytes:
 *
 *   - demand-paged first touch of .bss, of a grown stack and of fresh
 *     anonymous pages, i.e. loads and stores whose translation faults and
 *     is fixed up before the access is retried;
 *   - a heap that grows through brk() and a mapping that grows, shrinks
 *     and is recreated through mmap()/munmap(), so physical pages are
 *     handed out, returned and handed out again while the program runs;
 *   - syscalls that write into guest memory (read(), getcwd(), uname(),
 *     readlink()) followed by ordinary loads of what they wrote, and
 *     ordinary stores followed by a syscall that reads them back out;
 *   - unaligned and mixed-width accesses that straddle cache-line and page
 *     boundaries.
 *
 * Every line it prints is a fixed string or a value derived only from the
 * data it wrote, so two runs on two CPU models must produce byte-identical
 * output.  Nothing here prints an address, a time or a host detail.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <unistd.h>

#define PAGE 4096u

/*
 * gem5's RISC-V m5 ops are a custom-opcode instruction: quadrant 0x3,
 * opcode5 0x1e and the function in bits 31:25.  m5_work_begin is 0x5a and
 * takes its work and thread ids in a0/a1.  Emitting it inline keeps this
 * program free of a dependency on libm5.  Passing --workbegin lets a
 * configuration that stops on work items hand execution to another CPU
 * here; without the flag the program never executes it.
 */
static void
m5_work_begin(void)
{
#ifdef __riscv
    register unsigned long a0 __asm__("a0") = 0;
    register unsigned long a1 __asm__("a1") = 0;
    __asm__ __volatile__(".long 0xb400007b" : : "r"(a0), "r"(a1) : "memory");
#endif
}

/* .bss: untouched until the first store, one page at a time. */
#define BSS_PAGES 96
static unsigned char bss_area[BSS_PAGES * PAGE];

static int failures;

static void
check(const char *what, int ok)
{
    printf("%-28s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        failures++;
    }
}

/* A cheap, order-sensitive checksum; only its reproducibility matters. */
static uint64_t
digest(const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static unsigned char
pattern(size_t i)
{
    return (unsigned char)(i * 31u + (i >> 8) * 7u + 0x5au);
}

static void
fill(unsigned char *p, size_t n, size_t seed)
{
    for (size_t i = 0; i < n; i++) {
        p[i] = pattern(seed + i);
    }
}

static int
verify(const unsigned char *p, size_t n, size_t seed)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] != pattern(seed + i)) {
            return 0;
        }
    }
    return 1;
}

/* --- 1. demand-paged first touch ---------------------------------------- */

static void
test_bss(void)
{
    int zero = 1;
    /* First read of each page: it must be zero-filled on first touch. */
    for (size_t i = 0; i < BSS_PAGES; i++) {
        if (bss_area[i * PAGE] != 0 || bss_area[i * PAGE + PAGE - 1] != 0) {
            zero = 0;
        }
    }
    check("bss first touch zero", zero);

    fill(bss_area, sizeof(bss_area), 11);
    check("bss readback", verify(bss_area, sizeof(bss_area), 11));
    printf("bss digest                   %016llx\n",
           (unsigned long long)digest(bss_area, sizeof(bss_area)));
}

/* Recurse to push the stack into pages the process has never mapped. */
static uint64_t
stack_walk(int depth)
{
    volatile unsigned char frame[2048];
    for (size_t i = 0; i < sizeof(frame); i += 64) {
        frame[i] = (unsigned char)(depth + i);
    }
    if (depth > 0) {
        uint64_t below = stack_walk(depth - 1);
        return below * 31u + frame[0];
    }
    return frame[0];
}

static void
test_stack(void)
{
    uint64_t h = stack_walk(48);
    printf("stack growth digest          %016llx\n", (unsigned long long)h);
}

/* --- 2. heap growth through brk() --------------------------------------- */

static void
test_heap(void)
{
    enum
    {
        N = 12
    };
    unsigned char *blocks[N];
    int ok = 1;

    for (int i = 0; i < N; i++) {
        size_t n = (size_t)(i + 1) * 9013; /* not a multiple of a page */
        blocks[i] = (unsigned char *)malloc(n);
        if (!blocks[i]) {
            ok = 0;
            break;
        }
        fill(blocks[i], n, (size_t)i * 101);
    }
    check("heap allocation", ok);

    if (ok) {
        for (int i = 0; i < N; i++) {
            size_t n = (size_t)(i + 1) * 9013;
            if (!verify(blocks[i], n, (size_t)i * 101)) {
                ok = 0;
            }
        }
    }
    check("heap readback", ok);

    /* Free and reallocate: the allocator reuses and re-splits the break. */
    for (int i = 0; i < N; i += 2) {
        free(blocks[i]);
    }
    for (int i = 0; i < N; i += 2) {
        size_t n = (size_t)(i + 1) * 9013;
        blocks[i] = (unsigned char *)malloc(n);
        if (blocks[i]) {
            fill(blocks[i], n, (size_t)i * 101 + 5);
        }
    }
    uint64_t h = 0;
    for (int i = 0; i < N; i++) {
        size_t n = (size_t)(i + 1) * 9013;
        h = h * 1000003u + digest(blocks[i], n);
    }
    printf("heap digest                  %016llx\n", (unsigned long long)h);
    for (int i = 0; i < N; i++) {
        free(blocks[i]);
    }
}

/* --- 3. mmap growth, shrink and reuse ----------------------------------- */

static void
test_mmap(void)
{
    const size_t big = 64 * PAGE;
    unsigned char *m = (unsigned char *)mmap(
        NULL, big, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap anonymous", m != MAP_FAILED);
    if (m == MAP_FAILED) {
        return;
    }

    int zero = 1;
    for (size_t i = 0; i < big; i += PAGE) {
        if (m[i] != 0 || m[i + PAGE - 1] != 0) {
            zero = 0;
        }
    }
    check("mmap first touch zero", zero);

    fill(m, big, 7);
    check("mmap readback", verify(m, big, 7));
    printf("mmap digest                  %016llx\n",
           (unsigned long long)digest(m, big));

    /* Drop the tail, keep using the head: a mapping that shrinks. */
    check("munmap tail", munmap(m + big / 2, big / 2) == 0);
    check("mmap head survives", verify(m, big / 2, 7));

    /* A second mapping, taken after pages were returned to the pool. */
    unsigned char *m2 =
        (unsigned char *)mmap(NULL, 32 * PAGE, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap after munmap", m2 != MAP_FAILED);
    if (m2 != MAP_FAILED) {
        fill(m2, 32 * PAGE, 23);
        check("second mapping readback", verify(m2, 32 * PAGE, 23));
        printf("second mapping digest        %016llx\n",
               (unsigned long long)digest(m2, 32 * PAGE));
        /* The first mapping must be untouched by the second. */
        check("first mapping intact", verify(m, big / 2, 7));
        munmap(m2, 32 * PAGE);
    }
    munmap(m, big / 2);
}

/* --- 4. syscalls writing into, and reading out of, guest memory --------- */

static void
test_syscall_io(const char *path)
{
    const size_t len = 24 * PAGE + 137;
    unsigned char *out = (unsigned char *)malloc(len);
    check("io buffer", out != NULL);
    if (!out) {
        return;
    }
    fill(out, len, 3);

    /* The kernel reads bytes this program stored with ordinary stores. */
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    check("open scratch file", fd >= 0);
    if (fd < 0) {
        free(out);
        return;
    }
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, out + written, len - written);
        if (n <= 0) {
            break;
        }
        written += (size_t)n;
    }
    check("write out buffer", written == len);

    /*
     * Read it back into a mapping the CPU has never touched, so every
     * destination page is both demand-paged and filled by the kernel
     * rather than by a store.
     */
    unsigned char *in =
        (unsigned char *)mmap(NULL, 32 * PAGE, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap read target", in != MAP_FAILED);
    if (in != MAP_FAILED) {
        check("seek to start", lseek(fd, 0, SEEK_SET) == 0);
        size_t got = 0;
        while (got < len) {
            ssize_t n = read(fd, in + got, len - got);
            if (n <= 0) {
                break;
            }
            got += (size_t)n;
        }
        check("read into fresh pages", got == len);
        check("syscall-written bytes", memcmp(in, out, len) == 0);
        printf("file digest                  %016llx\n",
               (unsigned long long)digest(in, len));

        /* An unaligned, page-crossing read destination. */
        check("seek to start again", lseek(fd, 0, SEEK_SET) == 0);
        unsigned char *skew = in + PAGE - 3;
        ssize_t n = read(fd, skew, 3 * PAGE + 11);
        check("unaligned read target",
              n == 3 * PAGE + 11 && memcmp(skew, out, (size_t)n) == 0);
    }

    close(fd);
    unlink(path);
    if (in != MAP_FAILED) {
        munmap(in, 32 * PAGE);
    }
    free(out);
}

static void
test_syscall_fill(void)
{
    /* getcwd(), uname() and readlink() all write into guest memory. */
    char *cwd = (char *)mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap getcwd target", cwd != MAP_FAILED);
    if (cwd != MAP_FAILED) {
        char *r = getcwd(cwd, PAGE);
        check("getcwd", r == cwd && strlen(cwd) > 0 && cwd[0] == '/');
        munmap(cwd, PAGE);
    }

    struct utsname u;
    memset(&u, 0xa5, sizeof(u));
    check("uname", uname(&u) == 0 && strlen(u.sysname) > 0);

    char link[PAGE];
    memset(link, 0, sizeof(link));
    ssize_t n = readlink("/proc/self/exe", link, sizeof(link) - 1);
    check("readlink /proc/self/exe", n > 0 && link[0] == '/');
}

/* --- 5. unaligned and mixed-width accesses ------------------------------ */

static void
test_unaligned(void)
{
    const size_t n = 4 * PAGE;
    unsigned char *m = (unsigned char *)mmap(
        NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap unaligned area", m != MAP_FAILED);
    if (m == MAP_FAILED) {
        return;
    }

    /* Straddle every cache-line and page boundary in the mapping. */
    uint64_t h = 0;
    for (size_t off = PAGE - 9; off + 9 < n; off += 61) {
        uint64_t v = 0x0123456789abcdefULL ^ (uint64_t)off;
        memcpy(m + off, &v, sizeof(v));
        uint64_t back = 0;
        memcpy(&back, m + off, sizeof(back));
        if (back != v) {
            failures++;
        }
        h = h * 31u + back;

        uint32_t w = (uint32_t)off * 2654435761u;
        memcpy(m + off + 1, &w, sizeof(w));
        uint32_t wb = 0;
        memcpy(&wb, m + off + 1, sizeof(wb));
        if (wb != w) {
            failures++;
        }

        uint16_t s = (uint16_t)(off ^ 0xbeef);
        memcpy(m + off + 3, &s, sizeof(s));
        uint16_t sb = 0;
        memcpy(&sb, m + off + 3, sizeof(sb));
        if (sb != s) {
            failures++;
        }

        m[off + 7] = (unsigned char)off;
        if (m[off + 7] != (unsigned char)off) {
            failures++;
        }
    }
    printf("unaligned digest             %016llx\n", (unsigned long long)h);
    printf("unaligned area digest        %016llx\n",
           (unsigned long long)digest(m, n));
    munmap(m, n);
}

int
main(int argc, char **argv)
{
    const char *scratch = argc > 1 ? argv[1] : "se-memory.tmp";

    printf("se-memory start\n");
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--workbegin") == 0) {
            printf("workbegin\n");
            fflush(stdout);
            m5_work_begin();
        }
    }
    test_bss();
    test_stack();
    test_heap();
    test_mmap();
    test_syscall_io(scratch);
    test_syscall_fill();
    test_unaligned();
    printf("failures %d\n", failures);
    printf("se-memory done\n");
    return failures ? 1 : 0;
}
