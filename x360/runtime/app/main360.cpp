// Bloody Roar Extreme on the Xbox 360: entry point. The game's files sit beside default.xex (game:\) as they are on
// the disc, default.xbe among them; saves go under game:\hdd. Log: game:\mc3_360.log.
// Back + Start held for two seconds returns to the dashboard.
#include "app/app.h"

#include <algorithm>
#include <map>
#include <vector>

#include "core/crash.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "host.h"
#include "kernel/kernel.h"

extern "C" volatile long g_prof_req, g_prof_pos;  // MC3 loop profiler (kernel/guestlock.cpp)
extern "C" volatile uint32_t g_prof_ring[];

namespace game {
void CycleFrameSkip();
}
namespace kernel {
void LogRecentCalls(uint32_t id);
}

namespace app {

uint32_t Language() {
    // XC_LANGUAGE: 1 English, 2 Japanese, 3 German, 4 French, 5 Spanish, 6 Italian; the USA release has English only
    return 1;
}

bool Widescreen() {
    // MC3 bring-up (X-09): game:\aspect43.txt reports a 4:3 TV, and the title makes a 640x480 device instead of 720x480
    static const bool force43 = GetFileAttributesA("game:\\aspect43.txt") != DWORD(-1);
    if (force43) return false;
    XVIDEO_MODE mode;
    memset(&mode, 0, sizeof(mode));
    XGetVideoMode(&mode);
    return mode.fIsWideScreen != 0;
}

}  // namespace app

namespace {

// bring-up: where each guest thread is, every 5 s
const bool kWatch = false;

// the title's code (dispatch: 4700 functions over 00011000..0013F770)
const uint32_t kCodeLo = 0x00011000, kCodeHi = 0x00448000;  // MC3: .text to XPP (mcdub3-nx MC3_CODE_HI); JSRF ended at 0x18CB30

// a sampling profiler of the guest threads: the innermost return address on each one's stack, about a thousand times
// a second, the most frequent ones logged every 10 s (the time the title's own code takes, by caller)
struct Sampler {
    std::map<uint64_t, uint32_t> hits;  // thread id << 32 | return address
    std::map<uint32_t, uint32_t> totals;
    std::vector<kernel::ThreadRef> threads;
    int64_t next_list, next_log;
    Sampler() : next_list(0), next_log(0), hprof_read(0) {}

    // MC3 (2026-11-13): the loop profiler (cpu/x86_ops.h): once a millisecond the next back edge taken by any guest
    // thread records its host address; every 10 s the most frequent per thread are logged as "hprof:" (the generated
    // function holding the loop, found with bre360.map)
    long hprof_read;
    void SampleHost() { g_prof_req = 1; }
    void LogHost() {
        const long end = g_prof_pos;
        long from = hprof_read;
        if (end - from > 65536) from = end - 65536;
        hprof_read = end;
        std::map<uint32_t, std::map<uint32_t, uint32_t> > by;
        std::map<uint32_t, uint32_t> total;
        for (long i = from; i < end; i++) {
            const long k = i & 65535;
            const uint32_t tid = g_prof_ring[2 * k + 1], ra = g_prof_ring[2 * k];
            by[tid][ra]++;
            total[tid]++;
        }
        for (std::map<uint32_t, uint32_t>::iterator t = total.begin(); t != total.end(); ++t) {
            std::map<uint32_t, uint32_t>& h = by[t->first];
            std::vector<std::pair<uint32_t, uint32_t> > top;
            for (std::map<uint32_t, uint32_t>::iterator e = h.begin(); e != h.end(); ++e) top.push_back(std::make_pair(e->second, e->first));
            std::sort(top.rbegin(), top.rend());
            std::string line;
            for (size_t k = 0; k < top.size() && k < 80; k++) {
                char b[32];
                const char* f = reinterpret_cast<const char*>(uintptr_t(top[k].second));
                if (f && !strncmp(f, "__imp__", 7)) f += 7;
                _snprintf(b, sizeof(b), " %s:%u", f ? f : "?", top[k].first);
                line += b;
            }
            LOGI("hprof: host thread %X, %u samples:%s", t->first, t->second, line.c_str());
        }
    }

    void Sample(int64_t now) {
        // SampleHost();  // MC3: loop profiler off (2026-11-13, Xenia hangs while it was in)
        if (now >= next_list) {
            threads = kernel::Threads();
            next_list = now + 100000000ll;
        }
        for (size_t i = 0; i < threads.size(); i++) {
            X86Context* c = threads[i]->ctx;
            if (!c) continue;
            uint32_t esp = c->esp, ra = 0;
            if (esp < 0x1000) continue;
            {
                static uint64_t told = 0;
                uint32_t id = threads[i]->id, top = threads[i]->stack_top, low = top - threads[i]->stack_size - 0x1000;
                if ((esp > top || esp < low) && !(told >> ((id >> 2) & 63) & 1)) {
                    told |= uint64_t(1) << ((id >> 2) & 63);
                    LOGE("pilha: thread %X com esp %08X ebp %08X fora de [%08X, %08X]", id, esp, c->ebp, low, top);
                }
            }
            for (uint32_t k = 0; k < 48; k++) {
                // perto do fim da area de pilhas, esp + k * 4 cai em pagina sem commit (queda no Xenia, 2026-11-04)
                if (!mem::IsCommitted(esp + k * 4)) break;
                uint32_t v = kernel::R32(esp + k * 4);
                if (v >= kCodeLo && v < kCodeHi) {
                    ra = v;
                    break;
                }
            }
            hits[uint64_t(threads[i]->id) << 32 | ra]++;
            totals[threads[i]->id]++;
        }
        if (now < next_log) return;
        if (next_log) {
            // LogHost();
            for (std::map<uint32_t, uint32_t>::iterator t = totals.begin(); t != totals.end(); ++t) {
                std::vector<std::pair<uint32_t, uint32_t> > top;
                for (std::map<uint64_t, uint32_t>::iterator h = hits.begin(); h != hits.end(); ++h)
                    if (uint32_t(h->first >> 32) == t->first) top.push_back(std::make_pair(h->second, uint32_t(h->first)));
                std::sort(top.rbegin(), top.rend());
                std::string line;
                for (size_t k = 0; k < top.size() && k < 60; k++) {
                    char b[32];
                    _snprintf(b, sizeof(b), " %08X:%u%%", top[k].second, top[k].first * 100 / t->second);
                    line += b;
                }
                LOGI("prof: thread %X, %u samples:%s", t->first, t->second, line.c_str());
            }
            // MC3: per thread, esp, last indirect call and the first code addresses on its stack (a call chain guess)
            for (size_t i = 0; i < threads.size(); i++) {
                X86Context* c = threads[i]->ctx;
                if (!c) continue;
                std::string chain;
                for (uint32_t k = 0, found = 0; k < 256 && found < 8; k++) {
                    if (!mem::IsCommitted(c->esp + k * 4)) break;
                    uint32_t v = kernel::R32(c->esp + k * 4);
                    if (v >= kCodeLo && v < kCodeHi) {
                        char b[16];
                        _snprintf(b, sizeof(b), " %08X", v);
                        chain += b;
                        found++;
                    }
                }
                LOGI("prof: thread %X func %08X esp %08X last icall %08X kernel %s%u (%s) calls %u stack:%s", threads[i]->id, c->last_func, c->esp, c->last_icall,
                     (c->last_kernel >> 31) ? "IN " : "", c->last_kernel & 0x7FFFFFFF, kernel::ExportName(c->last_kernel & 0x7FFFFFFF),
                     c->kernel_calls, chain.c_str());
            }
            {
                std::string w;
                for (size_t i = 0; i < threads.size(); i++)
                    if (threads[i]->lock_wait) { char b[16]; _snprintf(b, sizeof(b), " %X", threads[i]->id); w += b; }
                LOGI("prof: guest lock held by %X, waiting:%s", kernel::g_guest_owner, w.c_str());
            }
            for (size_t i = 0; i < threads.size(); i++) {  // MC3: the KPCR/TLS reads of XAPI SetLastError (0x13187)
                X86Context* c = threads[i]->ctx;
                if (!c) continue;
                uint32_t fs = c->fs_base, sb = mem::IsCommitted(fs + 4) ? kernel::R32(fs + 4) : 0;
                int32_t idx = int32_t(kernel::R32(0x4CB678)); uint32_t tls = uint32_t(int32_t(sb) + idx * 4);
                LOGI("prof: thread %X fs %08X (%d) stackbase %08X tls slot %08X (%d) -> %08X", threads[i]->id, fs,
                     mem::IsCommitted(fs + 0x24) ? 1 : 0, sb, tls, mem::IsCommitted(tls) ? 1 : 0,
                     mem::IsCommitted(tls) ? kernel::R32(tls) : 0);
            }
            // MC3: the last backward jump each thread took (X86_BACKEDGE_AT), the loop a call-less spin sits in
            for (size_t i = 0; i < threads.size(); i++)
                if (threads[i]->ctx && threads[i]->ctx->last_loop)
                    LOGI("prof: thread %X last loop at %08X", threads[i]->id, threads[i]->ctx->last_loop);
            for (size_t i = 0; i < threads.size(); i++)
                if (threads[i]->wait_count)
                    LOGI("prof: thread %X waits on %u object(s), first type %u guest %08X, timeout %lld, called from %08X, for %lld ms",
                         threads[i]->id, threads[i]->wait_count, threads[i]->wait_type, threads[i]->wait_guest,
                         (long long)threads[i]->wait_timeout, threads[i]->wait_caller,
                         (long long)(host::NowNs() / 1000000 - threads[i]->wait_since_ms));
        }
        hits.clear();
        totals.clear();
        next_log = now + 10000000000ll;
    }
};

}  // namespace

void __cdecl main() {
    // MC3 X-19: each test switch gets a log of its own, so a run with one does not overwrite another's
    {
        std::string path = "game:\\mc3_360";
        static const char* const kSwitches[] = {"nodraw", "noaudio", "playintro"};
        for (int i = 0; i < 3; i++) {
            std::string f = std::string("game:\\") + kSwitches[i] + ".txt";
            if (GetFileAttributesA(f.c_str()) != DWORD(-1)) path += std::string("_") + kSwitches[i];
        }
        logx::Init(path + ".log");
    }
    crash::Install();
    LOGR("Midnight Club 3 Remix for Xbox 360 (runtime from JSRF/BRE), built " __DATE__ " " __TIME__);
    {   // MC3 (2026-11-17): the line above dates only this file; the XEX's own date tells one release from another
        WIN32_FILE_ATTRIBUTE_DATA fa;
        SYSTEMTIME st;
        if (GetFileAttributesExA("game:\\default.xex", GetFileExInfoStandard, &fa) && FileTimeToSystemTime(&fa.ftLastWriteTime, &st))
            LOGR("default.xex: %u bytes, saved %04u-%02u-%02u %02u:%02u:%02u (UTC)", unsigned(fa.nFileSizeLow), st.wYear, st.wMonth,
                 st.wDay, st.wHour, st.wMinute, st.wSecond);
    }
    MEMORYSTATUS ms;
    GlobalMemoryStatus(&ms);
    LOGI("memory: %u MB free of %u MB", unsigned(ms.dwAvailPhys >> 20), unsigned(ms.dwTotalPhys >> 20));

    if (!kernel::Boot("game:\\default.xbe", "game:", "game:\\hdd")) {
        LOGE("boot failed, see the lines above");
        logx::Flush();
        // stay up long enough for the log to be read, then leave
        Sleep(3000);
        XLaunchNewImage(XLAUNCH_KEYWORD_DEFAULT_APP, 0);
        return;
    }
    LOGI("boot: guest started");

    int64_t held_since = 0, start = host::NowNs(), next_dump = start + 5000000000ll, next_pad = 0;
    Sampler sampler;
    bool cap_held = false, fs_held = false, gm_held = false;
    for (;;) {
        Sleep(1);
        // MC3 (2026-11-16): the sampler every 10 ms, not every pass (~500 a second), on core 0 with the audio mixer
        static int64_t next_sample = 0;
        if (host::NowNs() >= next_sample) {
            next_sample = host::NowNs() + 10000000ll;
            sampler.Sample(host::NowNs());
        }
        crash::Pulse();
        if (host::NowNs() < next_pad) continue;
        next_pad = host::NowNs() + 50000000ll;
        // bring-up diagnostics: where each guest thread is, every 5 s for the first two minutes
        int64_t now = host::NowNs();
        {   // MC3 bring-up: game:utocap.txt with seconds since start ("69 75"), one frame capture at each
            static std::vector<double> cap_at;
            static bool cap_read = false;
            if (!cap_read) {
                cap_read = true;
                LOGI("mc3: reading game:\\skipdraw.txt");
                gpu::ReadSkipRules();  // MC3 X-23 diagnostic, game:\skipdraw.txt (gpu/xenos.cpp)
                if (FILE* f = fopen("game:\\autocap.txt", "rb")) {
                    double t;
                    while (fscanf(f, "%lf", &t) == 1) cap_at.push_back(t);
                    fclose(f);
                    if (cap_at.empty()) cap_at.push_back(10);
                }
            }
            if (!cap_at.empty() && double(now - start) / 1e9 >= cap_at.front()) {
                cap_at.erase(cap_at.begin());
                gpu::RequestCapture();
            }
        }
        if (kWatch && now >= next_dump && now - start < 120000000000ll) {
            next_dump += 5000000000ll;
            std::vector<kernel::ThreadRef> ts = kernel::Threads();
            LOGI("watch: %.0f s, %u guest threads", double(now - start) / 1e9, unsigned(ts.size()));
            for (size_t i = 0; i < ts.size(); i++)
                if (ts[i]->ctx) {
                    LOGI("watch: thread %X last indirect call %08X", ts[i]->id, ts[i]->ctx->last_icall);
                    kernel::LogGuestStack(*ts[i]->ctx, 24);
                }
        }
        input::Pad p = input::Poll(0);
        // JSRF: botoes de depuracao do BRE, so com game:\debug_buttons.txt. L3 e Back sao do jogo, e a captura congelava
        // a imagem por 6 a 38 s no console (tests/evidence/console_boot7).
        static const bool debug_buttons = GetFileAttributesA("game:\\debug_buttons.txt") != DWORD(-1);
        if (debug_buttons) {
            bool cap = (p.buttons & 0x40) != 0;
            if (cap && !cap_held) gpu::RequestCapture();
            cap_held = cap;
            bool gm = (p.buttons & 0x30) == 0x20;
            if (gm && !gm_held) gpu::CycleGamma();
            gm_held = gm;
        }
        bool exit_combo = (p.buttons & 0x30) == 0x30;  // start and back
        if (exit_combo) {
            if (!held_since) held_since = host::NowNs();
            if (host::NowNs() - held_since > 2000000000ll) {
                LOGI("mc3: saida pedida com Start + Back");
                kernel::Shutdown(0);
            }
        } else {
            held_since = 0;
        }
    }
}
