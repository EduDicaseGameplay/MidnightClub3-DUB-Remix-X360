// JSRF: so uma thread do jogo roda por vez, como no Xbox original, que tem um nucleo. O CRI ADX protege suas estruturas
// com SuspendThread e prioridades (ADXM_Lock, 0x13B0E0), o que nao protege nada com threads rodando em paralelo: erro
// ADXT -2 e congelamento intermitentes (2026-11-04). Mesma solucao do Unleashed (xbox_guest_lock em kernel_bridge.c):
// bloqueio unico, fila em ordem de chegada e fatia de 4 ms. A troca acontece em esperas do kernel, chamadas indiretas e
// saltos para tras do codigo gerado (X86_BACKEDGE). Quem pede SuspendThread agora para mesmo: a thread alvo so volta a
// rodar quando pega o bloqueio, e e ai que o pedido e cumprido.
#include "kernel/kernel.h"

#include <deque>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "host.h"

volatile long g_guest_preempt = 0;

namespace kernel {
volatile uint32_t g_guest_owner = 0;  // MC3

namespace {

const int64_t kSliceNs = 4000000;
// MC3 test (X-08): game:\nopreempt.txt never asks the holder to yield at a back edge
const bool kNoPreempt = GetFileAttributesA("game:\\nopreempt.txt") != DWORD(-1);

CRITICAL_SECTION* Cs() {
    static CRITICAL_SECTION cs;
    static volatile long state = 0;
    if (InterlockedCompareExchange(&state, 1, 0) == 0) {
        InitializeCriticalSection(&cs);
        state = 2;
    }
    while (state != 2) Sleep(0);
    return &cs;
}

bool g_busy = false;
volatile int g_owner_priority = 8;  // MC3: of the thread holding the guest lock
std::deque<HANDLE>* g_queue = NULL;
volatile int64_t g_handed = 0;
__declspec(thread) HANDLE t_event = NULL;
__declspec(thread) bool t_held = false;
__declspec(thread) int64_t t_wait_ns = 0;
// JSRF: a thread do jogo tem um nucleo so dela (render_thread.cpp). Dormir no evento e ser acordada pelo sistema custava cada
// troca de vez com as threads do CRI: no console, 12 a 20% do tempo dela na corrida de Dogenzaka (console_boot15). Ela espera
// a vez girando por ate 2 ms antes de dormir. As outras threads seguem dormindo, porque dividem o nucleo 0 com o audio.
__declspec(thread) bool t_spin = false;

void Take() {
    if (!t_event) t_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    CRITICAL_SECTION* cs = Cs();
    EnterCriticalSection(cs);
    if (!g_queue) g_queue = new std::deque<HANDLE>();
    Thread* me = Current();
    if (!g_busy) {
        g_busy = true;
        g_handed = host::NowNs();
        LeaveCriticalSection(cs);
    } else {
        g_queue->push_back(t_event);
        LeaveCriticalSection(cs);
        if (me) me->lock_wait = 1;
        int64_t t0 = host::NowNs();
        bool got = false;
        if (t_spin) {
            while (host::NowNs() - t0 < 2000000) {
                if (WaitForSingleObject(t_event, 0) == WAIT_OBJECT_0) {
                    got = true;
                    break;
                }
                YieldProcessor();
            }
        }
        // MC3 (2026-11-11): like the console's scheduler, a waiting thread cuts the holder's slice only at the same or a
        // higher priority. The title takes no lock in its heap and vectors while loading and counts on that: cut at
        // every 4 ms by lower priority threads, its loader corrupted a vector and copied over the XBE image (X-08). A
        // thread waiting 50 ms still gets its turn, as the NT kernel's boost of starved threads does.
        const int my_priority = me ? me->eff_priority : 8;
        if (!got)
            while (WaitForSingleObject(t_event, 4) == WAIT_TIMEOUT) {
                int64_t held = host::NowNs() - g_handed;
                if (!kNoPreempt && held >= kSliceNs && (my_priority >= g_owner_priority || held >= 50000000)) g_guest_preempt = 1;
            }
        t_wait_ns += host::NowNs() - t0;
        if (me) me->lock_wait = 0;
    }
    t_held = true;
    g_guest_owner = me ? me->id : 0xFFFFFFFF;
    g_owner_priority = me ? me->eff_priority : 8;
}

}  // namespace

uint32_t g_tls_index_addr = 0;
static uint32_t g_tls_index_value = 0;
void GuardTlsIndex(const char* where) {
    if (!g_tls_index_addr) return;
    uint32_t now = R32(g_tls_index_addr);
    if (!g_tls_index_value) {  // the XAPI entry sets it once, a small negative number of dwords below StackBase
        if (int32_t(now) < 0 && int32_t(now) > -4096) g_tls_index_value = now;
        return;
    }
    if (now == g_tls_index_value) return;
    static long told = 0;
    if (told < 8) {
        told++;
        Thread* me = Current();
        LOGW("x08: TLS index at %08X was %08X, put back to %08X (%s, thread %X, esp %08X)", g_tls_index_addr, now,
             g_tls_index_value, where, me ? me->id : 0, me && me->ctx ? me->ctx->esp : 0);
        if (me && me->ctx && told == 1) LogGuestStack(*me->ctx, 32);
    }
    W32(g_tls_index_addr, g_tls_index_value);
}

namespace {

void Give() {
    GuardTlsIndex("lock hand over");
    t_held = false;
    g_guest_owner = 0;
    CRITICAL_SECTION* cs = Cs();
    EnterCriticalSection(cs);
    g_guest_preempt = 0;
    if (g_queue->empty()) {
        g_busy = false;
    } else {
        HANDLE next = g_queue->front();
        g_queue->pop_front();
        g_handed = host::NowNs();
        ::SetEvent(next);
    }
    LeaveCriticalSection(cs);
}

}  // namespace

bool GuestHeld() { return t_held; }
void GuestSpinHere() { t_spin = true; }
int64_t GuestWaitNs() {
    int64_t v = t_wait_ns;
    t_wait_ns = 0;
    return v;
}

void GuestAcquire() {
    if (t_held) return;
    for (;;) {
        Take();
        Thread* t = Current();
        if (!t || t->suspend.load() <= 0) return;
        Give();
        std::unique_lock<std::mutex> lock(t->apc_lock);
        while (t->suspend.load() > 0) t->resume_cv.wait(lock);
    }
}

void GuestRelease() {
    if (t_held) Give();
}

// MC3 X-08 (2026-11-11): a back edge does not hand the guest lock over while this thread is inside code that must
// run as one piece (the title's heap, which takes no lock while loading: preempted halfway, its free list was left
// inconsistent, the next allocation returned null, and a vector copy from address 0 ran over the XBE image)
__declspec(thread) int t_no_yield = 0;
void NoYieldEnter() { t_no_yield++; }
void NoYieldLeave() { t_no_yield--; }

// MC3 X-08 (2026-11-11): on the console a thread that signals an object a higher priority thread waits on loses the
// processor right there, inside the kernel call. Here the woken thread only asked for the guest lock and took it at
// the signaller's next back edge, ~4 ms later and anywhere: the MC3 loader (priority 15) then ran in the middle of
// the main thread's (8) next vector update, which takes no lock. After a signal, a higher priority thread that was
// waiting gets the lock before the signaller goes on (it has 0.5 ms to wake up on the host).
void YieldAfterSignal(uint32_t guest) {
    if (!t_held || t_no_yield || !guest) return;
    Thread* me = Current();
    if (!me) return;
    // only threads that were waiting on the object just signalled (first object of their wait)
    std::vector<ThreadRef> ts = Threads(), higher;
    for (size_t i = 0; i < ts.size(); i++)
        if (ts[i].get() != me && ts[i]->eff_priority > me->eff_priority && ts[i]->wait_guest == guest &&
            (ts[i]->wait_count || ts[i]->lock_wait))
            higher.push_back(ts[i]);
    if (higher.empty()) return;
    int64_t t0 = host::NowNs();
    do {
        for (size_t i = 0; i < higher.size(); i++)
            if (higher[i]->lock_wait) {
                Give();
                GuestAcquire();
                return;
            }
        YieldProcessor();
    } while (host::NowNs() - t0 < 500000);
}

// MC3 X-19 (2026-11-11): on the Xbox a thread at DISPATCH_LEVEL or above is never switched out. XACT and DirectSound
// raise the IRQL (KfRaiseIrql, KeRaiseIrqlToDpcLevel) as their lock between the title's thread and the XACT worker;
// the runtime only kept the number, and a back edge handed the guest lock over inside such a region. The other
// thread then changed the same sound engine data (the console restarted 2 to 4 s after the title's music or a
// movie's sound started, with or without sound output). A raised thread keeps the lock; KfLowerIrql yields.
volatile long g_raised_keeps = 0;  // switches put off at a raised IRQL, for the log pulse
bool GuestRaised() {
    Thread* me = Current();
    return me && me->kpcr && G8(me->kpcr + 0x24) >= 2;
}

void GuestYield() {
    if (!t_held || t_no_yield) return;
    if (GuestRaised()) {
        Thread* me = Current();
        g_raised_keeps++;
        LOG_FIRST(8, "guestlock: thread %X at IRQL %u keeps the guest lock (last icall %08X, kernel %u)", me->id,
                  unsigned(G8(me->kpcr + 0x24)), me->ctx ? me->ctx->last_icall : 0,
                  me->ctx ? me->ctx->last_kernel & 0x7FFFFFFF : 0);
        return;
    }
    Give();
    GuestAcquire();
}

}  // namespace kernel

extern "C" void x86_guest_yield() { kernel::GuestYield(); }
// MC3 (2026-11-13), the loop profiler (cpu/x86_ops.h X86_BACKEDGE): the host address a back edge was taken from, by
// thread, in a ring main360 drains every 10 s. The address is in the generated function holding the loop (bre360.map).
extern "C" volatile long g_prof_req = 0;
extern "C" volatile long g_prof_pos = 0;
extern "C" volatile uint32_t g_prof_ring[2 * 65536] = {0};
extern "C" void* _ReturnAddress(void);
#pragma intrinsic(_ReturnAddress)
extern "C" __declspec(noinline) void x86_prof_hit(const char* func) {
    if (InterlockedExchange(&g_prof_req, 0) == 0) return;
    const long i = (InterlockedIncrement(&g_prof_pos) - 1) & 65535;
    g_prof_ring[2 * i] = uint32_t(uintptr_t(func));  // the generated function's name (a string in .rdata)
    g_prof_ring[2 * i + 1] = GetCurrentThreadId();
}

