// Xbox 360 build of pc-recomp/sdk/runtime/xdk/hle.h. Bloody Roar Extreme maps no argument registers ([abi] is
// empty), so a host implementation reads plain stdcall, fastcall or cdecl arguments.
#pragma once
#include "image_symbols.h"
#include "kernel/kernel.h"
#include <ppcintrinsics.h>

// Symbols are named in config/<image>.toml; the recompiler writes their addresses to image_symbols.h
#define X86_CAT_(a, b) a##b
#define X86_CAT(a, b) X86_CAT_(a, b)
#define SYM(name) X86_CAT(0x, SYM_##name)                  // address
#define GUEST(name) X86_CAT(sub_, SYM_##name)              // the function, a host override if there is one
#define GUEST_ORIG(name) X86_CAT(__imp__sub_, SYM_##name)  // the recompiled body

namespace hle {
// __fastcall arguments: ecx, edx, then the stack
struct FastArgs {
    X86Context& ctx;
    explicit FastArgs(X86Context& c) : ctx(c) {}
    uint32_t operator[](int i) const {
        return i == 0 ? ctx.ecx : i == 1 ? ctx.edx : ::kernel::R32(ctx.esp + 4 + 4 * uint32_t(i - 2));
    }

private:
    FastArgs& operator=(const FastArgs&);
};
}  // namespace hle

// MC3 (2026-11-12): calls and time of each host implementation, in the log every 5 s with the title's frames
// (hle::ProfReport): what the title's thread spends in the runtime, by function, not by the caller's address
namespace hle {
struct Prof {
    const char* name;
    uint64_t ticks;
    uint32_t calls;
    Prof* next;
    explicit Prof(const char* n);
    // MC3 (2026-11-14): one call in 16 is timed and counted 16 times: the time base read is a slow instruction on the
    // Xenon, and every one of the ~22,000 calls a race frame makes read it twice
    uint64_t Begin() const { return (calls & 15) ? 0 : __mftb(); }
    void Add(uint64_t t0) {
        if (t0) ticks += (__mftb() - t0) * 16;
        calls++;
    }
};
void ProfReport(uint32_t frames);
}  // namespace hle

#define HLE_BODY_(fn, guest, A, pops)                \
    static uint32_t fn(X86Context& ctx, const A& a); \
    static ::hle::Prof X86_CAT(prof_, fn)(#fn);      \
    X86_FUNC(guest) {                                \
        (void)base;                                  \
        uint64_t t0_ = X86_CAT(prof_, fn).Begin();   \
        A args(ctx);                                 \
        uint32_t r = fn(ctx, args);                  \
        ::kernel::Return(ctx, r, pops);              \
        X86_CAT(prof_, fn).Add(t0_);                 \
    }                                                \
    static uint32_t fn(X86Context& ctx, const A& a)

// Host implementation of a guest function: it replaces the recompiled body. a[i] is API argument i.
#define HLE_STDCALL(name, nargs) HLE_BODY_(hle_##name, GUEST(name), ::kernel::Args, (nargs))
#define HLE_FASTCALL(name, stack_args) HLE_BODY_(hle_##name, GUEST(name), ::hle::FastArgs, (stack_args))
#define HLE_CDECL(name) HLE_BODY_(hle_##name, GUEST(name), ::kernel::Args, 0)

// Runs before the recompiled function, which then sees its arguments untouched
#define HLE_WRAP(name, nargs)                                     \
    X86_EXTERN_FUNC(GUEST_ORIG(name));                            \
    static void hle_##name(X86Context& ctx, const ::kernel::Args& a); \
    static ::hle::Prof prof_hle_##name("hle_wrap_" #name);            \
    X86_FUNC(GUEST(name)) {                                       \
        uint64_t t0_ = prof_hle_##name.Begin();                   \
        ::kernel::Args args(ctx);                                 \
        hle_##name(ctx, args);                                    \
        GUEST_ORIG(name)(ctx, base);                              \
        prof_hle_##name.Add(t0_);                                 \
    }                                                             \
    static void hle_##name(X86Context& ctx, const ::kernel::Args& a)

// Calls a guest function by name with its API arguments: GUEST_CALL(name, ctx, a, b)
#define GUEST_CALL(name, ctx, ...) ::kernel::CallGuest(ctx, SYM(name), __VA_ARGS__)

// float arguments
template <class A>
inline float ArgF(const A& a, int i) {
    union { uint32_t u; float f; } c;
    c.u = a[i];
    return c.f;
}
inline float GF(uint32_t va) { return ::kernel::RF32(va); }
