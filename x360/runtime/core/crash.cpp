// MC3 (2026-11-11): see crash.h. A fault is logged with the PowerPC registers (find the host function with the
// address in build\bre360.map), the guest thread's x86 registers, its last indirect and kernel calls and the
// return addresses on its guest stack; then a short line per guest thread. The log is flushed and the console
// goes back to the dashboard instead of restarting.
#include "core/crash.h"

#include <eh.h>
#include <new.h>
#include <signal.h>
#include <stdlib.h>

#include "core/log.h"
#include "host.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

uint32_t PresentCount();  // xdk/d3d8.cpp
extern volatile uint32_t g_rt_op, g_rt_cmds;  // gpu/render_thread.cpp
extern volatile uint32_t g_ds_playing_now;     // xdk/dsound.cpp
namespace kernel {
extern volatile long g_raised_keeps;  // kernel/guestlock.cpp
}

namespace crash {

namespace {

volatile long g_faulted = 0;
int64_t g_start = 0;

void LogThreads() {
    LOGE("crash: guest lock held by thread %X", kernel::g_guest_owner);
    std::vector<kernel::ThreadRef> ts = kernel::Threads();
    for (size_t i = 0; i < ts.size(); i++) {
        X86Context* c = ts[i]->ctx;
        if (!c) continue;
        LOGE("crash: thread %X esp %08X last icall %08X kernel %s%u (%s) calls %u, waits %u on %08X", ts[i]->id, c->esp,
             c->last_icall, (c->last_kernel >> 31) ? "IN " : "", c->last_kernel & 0x7FFFFFFF,
             kernel::ExportName(c->last_kernel & 0x7FFFFFFF), c->kernel_calls, ts[i]->wait_count, ts[i]->wait_guest);
    }
}

void LogGuest(const char* what) {
    kernel::Thread* t = kernel::Current();
    if (t && t->ctx) {
        X86Context& c = *t->ctx;
        LOGE("crash: %s on guest thread %X, last icall %08X, kernel %s%u (%s)", what, t->id, c.last_icall,
             (c.last_kernel >> 31) ? "IN " : "", c.last_kernel & 0x7FFFFFFF,
             kernel::ExportName(c.last_kernel & 0x7FFFFFFF));
        kernel::LogGuestStack(c, 96);
    } else {
        LOGE("crash: %s on host thread %X (no guest thread)", what, unsigned(GetCurrentThreadId()));
    }
}

__declspec(noreturn) void Leave() {
    LogThreads();
    LOGE("crash: %.3f s after start, %u frames presented; log closed, back to the dashboard",
         double(host::NowNs() - g_start) / 1e9, ::PresentCount());
    logx::Flush();
    XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
    for (;;) Sleep(1000);
}

const char* CodeName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned data";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_BREAKPOINT: return "breakpoint (trap)";
    case EXCEPTION_IN_PAGE_ERROR: return "in page error";
    default: return "exception";
    }
}

// MC3 X-19: the boot faults once on purpose on a host thread of its own (Install): "crash: self-test ok" in the
// log proves the console delivers faults to the handler, so a restart with no "crash:" line is not one
// (2026-11-11, first console run: a write to 0x10 did not fault at all, the page is mapped there; the tests now use
// a reserved page with nothing committed, a division by zero and a misaligned 64 bit byte reversed load)
__declspec(thread) int t_selftest = 0;
volatile uint32_t g_zero = 0;
__declspec(noinline) void FaultReserved() {
    void* p = VirtualAlloc(NULL, 0x10000, MEM_RESERVE, PAGE_READWRITE);
    if (!p) {
        LOGE("crash: self-test 1 could not reserve a page");
        return;
    }
    *(volatile uint32_t*)((uint8_t*)p + 0x100) = 1;
}
__declspec(noinline) uint32_t FaultDivide() { return 1000u / g_zero; }
__declspec(noinline) uint64_t FaultMisaligned() {
    static __declspec(align(128)) uint8_t buf[256];
    return __loaddoublewordbytereverse(0, buf + 125);  // across a 128 byte cache line
}

LONG Report(EXCEPTION_POINTERS* info) {
    if (t_selftest) {
        LOGI("crash: self-test %d ok, %s (%08X) at %08X reached the handler", t_selftest,
             CodeName(info->ExceptionRecord->ExceptionCode), unsigned(info->ExceptionRecord->ExceptionCode),
             unsigned(uintptr_t(info->ExceptionRecord->ExceptionAddress)));
        t_selftest = 0;
        return EXCEPTION_EXECUTE_HANDLER;
    }
    if (InterlockedIncrement(&g_faulted) != 1) {  // a second fault while the first is written: wait for it
        for (;;) Sleep(1000);
    }
    EXCEPTION_RECORD* r = info->ExceptionRecord;
    CONTEXT* c = info->ContextRecord;
    uint32_t kind = r->NumberParameters > 0 ? uint32_t(r->ExceptionInformation[0]) : 0;
    uint32_t addr = r->NumberParameters > 1 ? uint32_t(r->ExceptionInformation[1]) : 0;
    LOGE("crash: %s (%08X) at %08X, %s %08X, host thread %X", CodeName(r->ExceptionCode), unsigned(r->ExceptionCode),
         unsigned(uintptr_t(r->ExceptionAddress)), kind ? "writing" : "reading", addr, unsigned(GetCurrentThreadId()));
    uint32_t base = uint32_t(uintptr_t(mem::Base()));
    if (addr >= base && addr - base < mem::kWindowSize)
        LOGE("crash: the address is in the guest window, offset %08X", addr - base);
    LOGE("crash: iar %08X lr %08X ctr %08X msr %08X cr %08X", unsigned(c->Iar), unsigned(c->Lr), unsigned(c->Ctr),
         unsigned(c->Msr), unsigned(c->Cr));
    const ULONGLONG* g = &c->Gpr0;
    for (int i = 0; i < 32; i += 8)
        LOGE("crash: r%-2d %08X %08X %08X %08X %08X %08X %08X %08X", i, unsigned(g[i]), unsigned(g[i + 1]),
             unsigned(g[i + 2]), unsigned(g[i + 3]), unsigned(g[i + 4]), unsigned(g[i + 5]), unsigned(g[i + 6]),
             unsigned(g[i + 7]));
    // host return addresses on the host stack (r1): the callers of the faulting function, for the map file
    {
        char line[256];
        int n = 0;
        uint32_t sp = uint32_t(g[1]);
        for (int k = 0, found = 0; k < 2048 && found < 12; k++) {
            uint32_t a = sp + uint32_t(k) * 4;
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery((void*)uintptr_t(a), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) break;
            uint32_t v = *(volatile uint32_t*)uintptr_t(a);
            if (v >= 0x82000000 && v < 0x84000000 && n < 230) {
                n += _snprintf(line + n, sizeof(line) - 1 - n, " %08X", v);
                found++;
            }
        }
        line[n] = 0;
        LOGE("crash: host stack:%s", line);
    }
    LogGuest(CodeName(r->ExceptionCode));
    Leave();
}

LONG WINAPI Unhandled(EXCEPTION_POINTERS* info) { return Report(info); }

void OnAbort(int) {
    LOGE("crash: abort()");
    LogGuest("abort");
    Leave();
}

void OnTerminate() {
    LOGE("crash: std::terminate (an exception nobody caught, often a failed allocation)");
    LogGuest("terminate");
    Leave();
}

void OnPurecall() {
    LOGE("crash: pure virtual call");
    LogGuest("pure virtual call");
    Leave();
}

int OnNoMemory(size_t size) {
    MEMORYSTATUS ms;
    GlobalMemoryStatus(&ms);
    LOGE("crash: host allocation of %u bytes failed, %u KB free", unsigned(size), unsigned(ms.dwAvailPhys >> 10));
    LogGuest("out of host memory");
    Leave();
}

void Run(std::function<void()>& fn) { fn(); }

}  // namespace

void Install() {
    g_start = host::NowNs();
    SetUnhandledExceptionFilter(Unhandled);
    // Xenia does not hand hardware faults to the title (it stops): its game folder has game:\noselftest.txt
    if (GetFileAttributesA("game:\\noselftest.txt") == DWORD(-1)) host::Spawn([] {
        std::function<void()> t1 = [] {
            t_selftest = 1;
            FaultReserved();
            LOGE("crash: self-test 1 FAILED: a write to a reserved page did not fault");
        };
        std::function<void()> t2 = [] {
            t_selftest = 2;
            uint32_t q = FaultDivide();
            LOGI("crash: self-test 2: integer division by zero does not fault on this CPU (result %u)", q);
        };
        std::function<void()> t3 = [] {
            t_selftest = 3;
            uint64_t v = FaultMisaligned();
            LOGI("crash: self-test 3: a misaligned byte reversed load across a cache line does not fault (%08X)",
                 unsigned(v));
        };
        std::function<void()> t0 = [] {
            t_selftest = 4;
            RaiseException(0xE0000001, 0, 0, NULL);
            LOGE("crash: self-test 4 FAILED: RaiseException did not reach the handler");
        };
        Guard(t0);
        Guard(t1);
        Guard(t2);
        Guard(t3);
        t_selftest = 0;
    }, "crash self-test", 0);
    signal(SIGABRT, OnAbort);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    set_terminate(OnTerminate);
    _set_purecall_handler(OnPurecall);
    _set_new_handler(OnNoMemory);
}

void Guard(std::function<void()>& fn) {
    __try {
        Run(fn);
    } __except (Report(GetExceptionInformation())) {
    }
}

void Pulse() {
    static int64_t next = 0;
    static uint32_t last_frames = 0;
    int64_t now = host::NowNs();
    if (now < next) return;
    double secs = double(now - g_start) / 1e9;
    next = now + (secs < 90 ? 250000000ll : 10000000000ll);
    uint32_t frames = ::PresentCount();
    MEMORYSTATUS ms;
    GlobalMemoryStatus(&ms);
    std::string line;
    std::vector<kernel::ThreadRef> ts = kernel::Threads();
    for (size_t i = 0; i < ts.size(); i++) {
        X86Context* c = ts[i]->ctx;
        if (!c) continue;
        char b[48];
        _snprintf(b, sizeof(b), " %X:%08X/%s%u", ts[i]->id, c->last_icall, (c->last_kernel >> 31) ? "in" : "",
                  c->last_kernel & 0x7FFFFFFF);
        line += b;
    }
    mem::Stats st = mem::Query();
    static const char* const kOps[] = {"state", "draw", "clear", "copy", "present", "sync", "flush", "gamma",
                                       "clear report", "get report", "init", "dirty"};
    uint32_t op = g_rt_op;
    static uint32_t last_cmds = 0;
    uint32_t cmds = g_rt_cmds;
    LOGI("pulse: renderer %s %s, %u commands (+%u); sounds playing %u; switches kept at raised IRQL %ld",
         (op & 0x80) ? "in" : "after", (op & 0x7F) < 12 ? kOps[op & 0x7F] : "nothing yet", cmds, cmds - last_cmds,
         g_ds_playing_now, kernel::g_raised_keeps);
    last_cmds = cmds;
    LOGI("pulse: %.1f s, frame %u (+%u), lock %X, host free %u KB, guest free %u KB (virtual %u KB, contiguous %u KB, "
         "system %u KB), threads (last icall/kernel):%s", secs, frames, frames - last_frames, kernel::g_guest_owner,
         unsigned(ms.dwAvailPhys >> 10), st.available_pages * 4, st.virtual_committed * 4, st.contiguous_used * 4,
         st.system_used * 4, line.c_str());
    last_frames = frames;
    // MC3 X-19: the stack report (host::StackReport) is no longer called: measured on the console at 198K of 4096K
    // for the title's thread, and it read the freed stack of a thread that had exited (an access violation there,
    // console run 12, 2026-11-11)
    // test of this file: game:\crashtest.txt makes the main thread fault 5 s in
    static const bool test = GetFileAttributesA("game:\\crashtest.txt") != DWORD(-1);
    if (test && secs >= 5) {
        LOGI("crash: test fault (game:\\crashtest.txt)");
        *(volatile uint32_t*)uintptr_t(0x10) = 1;
    }
}

}  // namespace crash
