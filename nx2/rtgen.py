"""a64-kit's guest model (rt/a64.h, rt/a64_rt.c) with a 64-bit guest window, written next to the lifted code.

a64-kit maps all guest memory below 4 GB (`A64_MEM + (uint32_t)addr`): its own loader picks every address. MKW
does not fit: a 39-bit address space, ~9.5 GiB of planned memory on the 12 GB layout, 2x 5.7 GiB sparse GPU
regions. With A64_ADDR64 (defined by the generated header) an access is `A64_MEM + addr` over a reservation of the
whole 39-bit space (Eden's fastmem arena, or our own), and lookups of code addresses refuse anything above 4 GB
instead of truncating it (code always lies below: Eden loads it at 0x80000000).

It also adds the interrupt flag and A64_POLL that nx2/poll.py's loop-header polls use.

The changes are text substitutions on the vendored files, each required to match exactly once, so a refreshed
a64-kit that moved these lines fails here loudly instead of silently keeping the 32-bit window.
"""
import os

import kit

POLL_DECL = r"""
/* (nx2/poll.py) loop back-edge interrupt polls: A64_POLLING builds clear c->poll and call a64_poll_cur, the backend's,
   when it is set. The call is handed nothing on purpose: the context parameter is noescape (A64_NOESC) and never
   captured, so a call with no pointer argument cannot touch *c, and the compiler keeps guest registers in host registers
   across the loop. (a64_poll(c), the old form, made every loop body reload and store them: a call that takes c may change
   any of it.) The poll sits before the backward goto, after the body's stores: at the loop header, a call C cannot mark
   as returning (LLVM's willreturn) left the stores after it not guaranteed to execute, and nothing was promoted.
   a64_poll_cur finds the thread's context itself (host/mkw_glue.c, hos_ctx). A64_POLL_CTX keeps the old form for the
   emulator-hosted DLL (nx2/rt/eden_shim.c), whose host's poll takes the context. */
void a64_poll(Ctx *A64_NOESC c);
void a64_poll_cur(void);
#if defined(A64_POLLING) && defined(A64_POLL_CTX)
#define A64_POLL(c) do { if (__builtin_expect(__atomic_load_n(&(c)->poll, __ATOMIC_RELAXED) != 0, 0)) a64_poll(c); } while (0)
#elif defined(A64_POLLING)
#define A64_POLL(c) do { if (__builtin_expect(__atomic_load_n(&(c)->poll, __ATOMIC_RELAXED) != 0, 0)) { \
    __atomic_store_n(&(c)->poll, 0, __ATOMIC_RELAXED); a64_poll_cur(); } } while (0)
#else
#define A64_POLL(c) ((void)0)
#endif"""

ADDR64 = r"""#ifndef A64_ADDR64
#define A64_ADDR64 1   /* (nx2/rtgen.py: 64-bit guest window) */
#endif
#if A64_ADDR64
#define A64_PTR(a) (A64_MEM + (uint64_t)(a))
#define A64_DIRTY_PAGES (1ull << 27)   /* (nx2/rtgen.py) A64_WRITE_TRACK: a mark per 4 KB page of the 512 GB window */
#else
#define A64_DIRTY_PAGES (1ull << 20)
#define A64_PTR(a) (A64_MEM + (uint32_t)(a))
#endif"""

RET_MISMATCH = ("void a64_ret_mismatch(Ctx *A64_NOESC c, uint64_t pc);         "
                "/* RET to something other than the caller (debug builds) */")
THREAD = "    void *thread;               /* the OS layer's thread object */\n} GuestContext;"
LOOKUP = "    uint32_t a = (uint32_t)addr, lo = 0, hi = a64_fn_count;"
# (the soft write watch's table at the window's size, the address masked into it: a stray pointer marks a wrong
# page, never memory past the table)
MARK_NATIVE = r"""extern uint8_t a64_dirty[1u << 20];
/* (Tried 2026-10-21: reading a mark first and writing it only when clear, against cache lines moving between cores:
   game threads +1.1 ms a frame, the code 18 MB bigger. Not kept.) */
#define A64_MARK(a, n) (__atomic_store_n(&a64_dirty[(uint32_t)(a) >> 12], 1, __ATOMIC_RELEASE), \
                        __atomic_store_n(&a64_dirty[(uint32_t)((a) + (n) - 1) >> 12], 1, __ATOMIC_RELEASE))"""
MARK_NATIVE_64 = r"""extern uint8_t *a64_dirty;   /* (a64_rt.c) reserved, committed as pages of it are stored to */
void a64_dirty_init(void);
/* (Tried 2026-10-21: reading a mark first and writing it only when clear, against cache lines moving between cores:
   game threads +1.1 ms a frame, the code 18 MB bigger. Not kept.) */
#define A64_DIRTY_AT(a) (((uint64_t)(a) >> 12) & (A64_DIRTY_PAGES - 1))
#define A64_MARK(a, n) (__atomic_store_n(&a64_dirty[A64_DIRTY_AT(a)], 1, __ATOMIC_RELEASE), \
                        __atomic_store_n(&a64_dirty[A64_DIRTY_AT((a) + (n) - 1)], 1, __ATOMIC_RELEASE))"""

SUBST = {
    "a64.h": [
        (THREAD, THREAD.replace("\n} GuestContext;", "\n    uint32_t poll;              "
                                "/* (nx2/rtgen.py) interrupt request: A64_POLL clears it and calls a64_poll_cur when set */\n} GuestContext;")),
        (RET_MISMATCH, RET_MISMATCH + POLL_DECL),
        ("#define A64_PTR(a) (A64_MEM + (uint32_t)(a))", ADDR64),
        (MARK_NATIVE, MARK_NATIVE_64),
    ],
    "a64_rt.c": [
        (LOOKUP, "#if A64_ADDR64\n    if (addr >> 32) return NULL;   /* (code lies below 4 GB: never a truncated match) */\n"
                 "#endif\n" + LOOKUP),
        ("uint8_t a64_dirty[1u << 20];", r"""/* One byte per guest page of the 512 GB window is 128 MB. A static array is charged to the Windows
   commit in full, beside a 5 GB job that the forest already fills. The table is reserved and a page of it
   (16 MB of guest addresses) is committed on the first store or scan that touches it. */
#if defined(_WIN32)
uint8_t *a64_dirty;
static LONG CALLBACK a64_dirty_fault(EXCEPTION_POINTERS *ep) {
    if (!ep || !ep->ExceptionRecord || ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->NumberParameters < 2 || !a64_dirty) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t addr = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
    const uintptr_t base = (uintptr_t)a64_dirty;
    if (addr < base || addr >= base + A64_DIRTY_PAGES) return EXCEPTION_CONTINUE_SEARCH;
    if (!VirtualAlloc((void *)(addr & ~(uintptr_t)0xFFF), 0x1000, MEM_COMMIT, PAGE_READWRITE))
        return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_CONTINUE_EXECUTION;
}
void a64_dirty_init(void) {
    if (a64_dirty) return;
    a64_dirty = (uint8_t *)VirtualAlloc(NULL, A64_DIRTY_PAGES, MEM_RESERVE, PAGE_READWRITE);
    if (a64_dirty) AddVectoredExceptionHandler(1, a64_dirty_fault);
}
__attribute__((constructor(101))) static void a64_dirty_construct(void) { a64_dirty_init(); }
#elif defined(__EMSCRIPTEN__)
uint8_t a64_dirty[1u << 20];
void a64_dirty_init(void) {}
#else
static uint8_t a64_dirty_store[A64_DIRTY_PAGES];
uint8_t *a64_dirty = a64_dirty_store;
void a64_dirty_init(void) {}
#endif"""),
    ],
}


def write(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    for name, subs in SUBST.items():
        text = open(os.path.join(kit.KIT, "rt", name), encoding="utf-8").read()
        for old, new in subs:
            n = text.count(old)
            if n != 1:
                raise SystemExit("rtgen: %r found %d times in a64-kit rt/%s (expected once): update nx2/rtgen.py"
                                 % (old[:60], n, name))
            text = text.replace(old, new)
        head = "/* Generated by mkw-recomp nx2/rtgen.py from a64-kit rt/%s (64-bit guest window). Do not edit. */\n" % name
        with open(os.path.join(out_dir, name), "w", newline="\n", encoding="utf-8") as f:
            f.write(head + text)
    return out_dir
