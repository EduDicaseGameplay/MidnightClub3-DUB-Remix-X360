// Midnight Club 3: DUB Edition Remix (USA) on the Xbox 360. A strong definition replaces the recompiled function at
// that address. Started from JSRF's game file (referencia_jsrf/jsrf.cpp): every JSRF address was removed, only the
// entry points the runtime calls remain. MC3's own fixes go here, each with its evidence.
#include <string>

#include "cpu/x86_ops.h"
#include "core/log.h"
#include "core/input.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

using kernel::R32;
using kernel::W32;

namespace game {
// called by app/main360.cpp (JSRF's frame-skip switch); MC3 pacing is not decided yet
void CycleFrameSkip() {}
// called by xdk/xapi.cpp for port 0 on every poll; MC3 has no map picker yet
void MapPickerPad(input::Pad&) {}
}  // namespace game

// XAPI boot (0x13295 -> 0x13231 -> 0x1736C): when [Prcb + 0x250] is 0 the XAPI finds the kernel's INIT section and
// installs a GDT descriptor: sgdt, two xchg into the GDT between cli/sti, jmp far 0x0008:0x1738C to reload CS, then
// "mov eax, ecx; ret 8" returns the old descriptor. There is no GDT here: sgdt is ignored, so eax held stack garbage
// and the xchg wrote to a random guest address, and the far jump became an indirect call to 0 (first Xenia boot,
// 2026-11-10: thread 14 spun at 100% right after this call, log "prof: thread 14 last loop at 00013354").
// Replaced: no descriptor change, old descriptor 0, stdcall with 2 arguments.
//
// The whole routine at 0x13295 (called once from 0x1292B, the XAPI's main thread start) only makes sense on an Xbox:
// it patches one instruction of the kernel's code for kernel builds 0xF68/0xFC2/0xFC7/0x12D1-0x12D2, then jumps
// (inside the same function, so the 0x1736C override alone is not reached) to 0x13231, which reads the kernel's PE
// header at 0x8001003C. Here 0x8001003C folds to the XBE header and the next read lands on an unmapped address of
// the guest window; the second Xenia boot still spun at "last loop at 00013354" with the 0x1736C override present
// and never logged it. Nothing in it applies to this runtime, so it returns at once (plain ret, no arguments).
X86_FUNC(sub_00013295) {
    LOGI("mc3: XAPI kernel patch and GDT setup skipped (0x13295)");
    ctx.esp += 4;
}
X86_FUNC(sub_0001736C) {
    static bool told = false;
    if (!told) { told = true; LOGI("mc3: XAPI GDT descriptor install skipped (0x1736C)"); }
    ctx.eax = 0;
    ctx.edx = 0;
    ctx.esp += 4 + 8;  // return address and the two arguments (ret 8)
}

// WMA music decoder (WMADEC): the overlap-add loops of 16 samples are do-while. At 0x42CDC9 / 0x42D129 the block count
// is loaded into eax; the loops end with "dec eax; jnz" at 0x42D0A1 / 0x42D421 and exit to 0x42D0A8 / 0x42D428. With a
// count of 0 the first dec makes -1 and the loop runs ~4 billion times past its buffers (crash at a song change).
// Cause found by mc3port (SickSartori, MIT, CHANGELOG 0.3.5, issue #13); bytes re-checked in this XBE; same fix as
// port/templates/game/src/game/wma_fix.cpp of the PC reference. Wired with [[hook]] in pc-recomp/config/game.toml.
static uint32_t g_wma_skips = 0;
static bool WmaSkip(X86Context& ctx) {
    if (ctx.eax != 0) return false;
    if (g_wma_skips++ < 8) LOGI("wma: empty overlap-add loop skipped (count 0)");
    return true;
}
bool mc3_wma_overlap_first(X86Context& ctx, uint8_t*) { return WmaSkip(ctx); }
bool mc3_wma_overlap_second(X86Context& ctx, uint8_t*) { return WmaSkip(ctx); }

// MC3 X-08 (2026-11-11): std::vector<12 byte element>::insert(pos, value) at 0x27B8C0 (ecx the vector: +4 begin,
// +8 end, +0C capacity end; [esp+8] pos). During loading one vector arrived inconsistent, and the element copy of
// the insert (0x278590, which stops only when first == last, in 12 byte steps) then ran over the whole guest memory,
// the XBE's TLS index at 0x4CB678 among it, sometimes for minutes. Such a vector is logged and taken as empty, so the
// insert stays inside its own buffer. What leaves it inconsistent is still open (X-08).
void mc3_vector_guard(X86Context& ctx, uint8_t*) {
    uint32_t v = ctx.ecx, first = R32(v + 4), last = R32(v + 8), cap = R32(v + 12), pos = R32(ctx.esp + 8);
    bool bad;
    if (!first) bad = last || cap || pos;
    else bad = last < first || cap < last || pos < first || pos > last || (last - first) % 12 || (cap - first) % 12 ||
               (pos - first) % 12;
    if (!bad) return;
    static int told = 0;
    if (told < 8) {
        told++;
        LOGW("x08: vector %08X inconsistent: begin %08X end %08X capacity %08X, insert at %08X; taken as empty", v, first,
             last, cap, pos);
        if (told == 1) kernel::LogGuestStack(ctx, 48);
    }
    W32(v + 4, 0);
    W32(v + 8, 0);
    W32(v + 12, 0);
    W32(ctx.esp + 8, 0);
}
