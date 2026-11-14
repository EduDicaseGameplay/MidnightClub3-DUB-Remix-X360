// MC3 (2026-11-13): career race results and events, the PC reference's safe listener broadcast
// (port/templates/game/src/game/hooks.cpp, sub_002793C0 / sub_00279430; same project, adapted to VS2010 and this
// runtime). mcPgCareer::ReportRaceResult() (0x277EE0) broadcasts through a list of 12 byte listener entries
// {u16 id, u32 key, object*} at [this+0x78 .. this+0x7C], calling the entry at index [this+0x94] first. A callback can
// change or free entries while the list is walked: the list is copied first, each object, vtable and function is
// checked before its call, and listeners registered meanwhile are called at the end (race start callbacks need it).
// Not yet seen failing here: no test so far finished a career race (AUDITORIA_PC_X360.md).
#include <string.h>

#include "cpu/x86_ops.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

using kernel::R32;
using kernel::W32;

namespace {
const uint32_t kListenerMax = 512;

bool ListenerRam(uint32_t va) { return !(va & 3u) && va >= 0x10000u && va < 0x84000000u && mem::IsCommitted(va); }

bool ListenerValid(uint32_t obj, uint32_t slot) {
    if (!ListenerRam(obj)) return false;
    uint32_t vt = R32(obj);
    if ((vt & 3u) || vt < 0x004476E0u || vt >= 0x004A8140u || !mem::IsCommitted(vt)) return false;
    uint32_t fn = R32(vt + slot);
    return fn >= 0x00012000u && fn < 0x004476C8u;
}

bool ListenerLive(uint32_t self, uint32_t obj, uint16_t id, uint32_t key) {
    uint32_t begin = R32(self + 0x78), end = R32(self + 0x7C);
    if (!ListenerRam(begin) || end < begin || end - begin > 12u * 4096u) return false;
    for (uint32_t e = begin; e + 12u <= end; e += 12u)
        if (R32(e + 8) == obj && kernel::R16(e) == id && R32(e + 4) == key) return true;
    return false;
}

void ListenerPrune(uint32_t self, uint32_t slot) {
    if (!ListenerRam(self)) return;
    uint32_t begin = R32(self + 0x78), end = R32(self + 0x7C), idx = R32(self + 0x94);
    if (!ListenerRam(begin) || end < begin || end - begin > 12u * 4096u || (end - begin) % 12u != 0) return;
    uint32_t wr = begin, new_idx = 0xFFFFFFFFu, i = 0;
    for (uint32_t rd = begin; rd < end; rd += 12u, ++i) {
        uint32_t obj = R32(rd + 8);
        if (!ListenerValid(obj, slot)) continue;
        if (i == idx) new_idx = (wr - begin) / 12u;
        if (wr != rd) {
            W32(wr, R32(rd));
            W32(wr + 4, R32(rd + 4));
            W32(wr + 8, R32(rd + 8));
        }
        wr += 12u;
    }
    if (wr != end) {
        static int told = 0;
        if (told++ < 8) LOGW("career: %u stale listener(s) dropped from %08X", (end - wr) / 12u, self);
        W32(self + 0x7C, wr);
        if (idx != 0xFFFFFFFFu) W32(self + 0x94, new_idx);
    }
}

void ListenerCall(X86Context& ctx, uint8_t* base, uint32_t obj, uint32_t slot, uint32_t arg) {
    uint32_t fn_va = R32(R32(obj) + slot);
    uint32_t esp0 = ctx.esp, bx = ctx.ebx, si = ctx.esi, di = ctx.edi, bp = ctx.ebp;
    X86_PUSH32(arg);
    X86_PUSH32(0x2793F5u);
    ctx.ecx = obj;
    x86_icall(ctx, base, fn_va);
    ctx.esp = esp0;
    ctx.ebx = bx;
    ctx.esi = si;
    ctx.edi = di;
    ctx.ebp = bp;
}

struct Entry {
    uint16_t id;
    uint32_t key, obj;
};

void ListenerBroadcast(X86Context& ctx, uint8_t* base, uint32_t slot) {
    uint32_t self = ctx.ecx;
    uint32_t arg = R32(ctx.esp + 4);
    uint32_t saved_ebx = ctx.ebx, saved_esi = ctx.esi, saved_edi = ctx.edi, saved_ebp = ctx.ebp;
    static Entry snap[kListenerMax];  // the game's main thread only
    {
        static bool told = false;
        if (!told) { told = true; LOGI("career: listener broadcast (slot %X) from %08X", slot, R32(ctx.esp)); }
    }
    ListenerPrune(self, slot);
    uint32_t begin = R32(self + 0x78), end = R32(self + 0x7C), idx = R32(self + 0x94);
    uint32_t n = (ListenerRam(begin) && end >= begin) ? (end - begin) / 12u : 0;
    if (n > kListenerMax) n = kListenerMax;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t e = begin + 12u * i;
        snap[i].id = kernel::R16(e);
        snap[i].key = R32(e + 4);
        snap[i].obj = R32(e + 8);
    }
    uint16_t cur_id = 0;
    uint32_t cur_key = 0;
    if (idx != 0xFFFFFFFFu && idx < n) {
        cur_id = snap[idx].id;
        cur_key = snap[idx].key;
        if (ListenerValid(snap[idx].obj, slot)) ListenerCall(ctx, base, snap[idx].obj, slot, arg);
    }
    for (uint32_t i = 0; i < n; ++i) {
        if (snap[i].id == cur_id && snap[i].key == cur_key) continue;
        if (!ListenerLive(self, snap[i].obj, snap[i].id, snap[i].key)) continue;
        if (ListenerValid(snap[i].obj, slot)) ListenerCall(ctx, base, snap[i].obj, slot, arg);
    }
    for (int round = 0; round < 8 && n < kListenerMax; ++round) {
        uint32_t b = R32(self + 0x78), en = R32(self + 0x7C), added = 0;
        if (!ListenerRam(b) || en < b || en - b > 12u * 4096u) break;
        for (uint32_t e = b; e + 12u <= en && n < kListenerMax; e += 12u) {
            uint16_t id = kernel::R16(e);
            uint32_t key = R32(e + 4), obj = R32(e + 8);
            bool seen = (id == cur_id && key == cur_key);
            for (uint32_t k = 0; k < n && !seen; ++k) seen = (snap[k].obj == obj && snap[k].id == id && snap[k].key == key);
            if (seen) continue;
            snap[n].id = id;
            snap[n].key = key;
            snap[n].obj = obj;
            n++;
            ++added;
            if (ListenerValid(obj, slot)) ListenerCall(ctx, base, obj, slot, arg);
        }
        if (!added) break;
    }
    ctx.ebx = saved_ebx;
    ctx.esi = saved_esi;
    ctx.edi = saved_edi;
    ctx.ebp = saved_ebp;
    ctx.esp += 8;  // ret 0x04
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_002793C0);
X86_FUNC(sub_002793C0) { ListenerBroadcast(ctx, base, 0x28); }

X86_EXTERN_FUNC(__imp__sub_00279430);
X86_FUNC(sub_00279430) { ListenerBroadcast(ctx, base, 0x2C); }
