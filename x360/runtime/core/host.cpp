// Clock, sleep and threads on the Xbox 360 (see shim/host.h)
#include "host.h"

#include <string.h>

#include "core/crash.h"

namespace host {

static int64_t Frequency() {
    static int64_t f = 0;
    if (!f) {
        LARGE_INTEGER li;
        QueryPerformanceFrequency(&li);
        f = li.QuadPart;
    }
    return f;
}

int64_t NowNs() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    int64_t f = Frequency();
    return c.QuadPart / f * 1000000000ll + c.QuadPart % f * 1000000000ll / f;
}

int64_t SystemTime100ns() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return int64_t((uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
}

void SleepNs(int64_t ns) {
    if (ns <= 0) {
        SwitchToThread();
        return;
    }
    // Sleep has millisecond resolution: the last part spins on the clock
    int64_t end = NowNs() + ns;
    if (ns > 2000000) Sleep(DWORD((ns - 1000000) / 1000000));
    while (NowNs() < end) SwitchToThread();
}

namespace {

// guest and host threads without a hardware thread of their own alternate between hardware threads 0 and 1
volatile LONG g_next_core = 0;

// recompiled code nests a host frame per guest call: give it room
const uint32_t kStackBytes = 4u << 20;

struct SpawnArgs {
    std::function<void()> fn;
    char name[24];
};

// MC3 X-19 (2026-11-11): a stack that runs out on the console takes the title down with no handler able to run
// (no stack left), and Xenia keeps its stacks in emulated memory where running past the end harms nothing. Each
// thread paints its stack at start; StackReport finds how far down the paint was overwritten.
const uint32_t kPaint = 0x5AA5C33Cu;
struct StackInfo {
    char name[24];
    uintptr_t top, bottom;
};
StackInfo g_stacks[64];
volatile LONG g_nstacks = 0;

__declspec(noinline) void PaintStack(const char* name) {
    volatile uint32_t here = 0;
    uintptr_t top = (reinterpret_cast<uintptr_t>(&here) + 0xFFF) & ~uintptr_t(0xFFF);
    uintptr_t bottom = top - kStackBytes + 0x10000;  // the system's guard and reserve stay untouched
    uintptr_t from = (reinterpret_cast<uintptr_t>(&here) - 0x4000) & ~uintptr_t(15);  // below this frame and its callees
    for (uintptr_t a = bottom; a < from; a += 4) *reinterpret_cast<volatile uint32_t*>(a) = kPaint;
    LONG i = InterlockedIncrement(&g_nstacks) - 1;
    if (i < 64) {
        strncpy(g_stacks[i].name, name, sizeof(g_stacks[i].name) - 1);
        g_stacks[i].bottom = bottom;
        g_stacks[i].top = top;
    }
}

DWORD WINAPI Entry(LPVOID p) {
    SpawnArgs* a = static_cast<SpawnArgs*>(p);
    // PaintStack(a->name);  // MC3 X-19 measurement, done (198K of 4096K at most on the console)
    crash::Guard(a->fn);  // MC3: a fault on this thread is logged (core/crash.cpp)
    delete a;
    return 0;
}

}  // namespace

void Spawn(std::function<void()> fn, const char* name, int core) {
    SpawnArgs* a = new SpawnArgs;
    a->fn = fn;
    memset(a->name, 0, sizeof(a->name));
    strncpy(a->name, name ? name : "?", sizeof(a->name) - 1);
    HANDLE h = CreateThread(NULL, kStackBytes, Entry, a, CREATE_SUSPENDED, NULL);
    if (!h) {
        delete a;
        return;
    }
    // core 0 (hardware threads 0 and 1) runs everything light and audio: core 1 is the title's thread's and core 2
    // the renderer's (gpu/render_thread.cpp)
    if (core < 0) core = int(InterlockedIncrement(&g_next_core) & 1);
    XSetThreadProcessor(h, DWORD(core));
    ResumeThread(h);
    CloseHandle(h);
}

void StackReport(char* out, size_t size) {
    size_t n = 0;
    out[0] = 0;
    LONG count = g_nstacks < 64 ? g_nstacks : 64;
    for (LONG i = 0; i < count && n + 40 < size; i++) {
        const StackInfo& s = g_stacks[i];
        uintptr_t a = s.bottom;
        while (a < s.top && *reinterpret_cast<volatile uint32_t*>(a) == kPaint) a += 64;
        unsigned used_kb = unsigned((s.top - a) >> 10);
        n += _snprintf(out + n, size - n - 1, " %s:%uK", s.name, used_kb);
    }
    out[size - 1] = 0;
}

}  // namespace host
