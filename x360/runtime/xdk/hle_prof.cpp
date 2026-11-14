// MC3 (2026-11-12): the title's time in the host implementations, by function (hle.h, HLE_BODY_), and in kernel
// calls (cpu/dispatch.cpp), logged by the draw statistics every 5 s on the title's thread
#include <algorithm>
#include <string>
#include <vector>

#include "core/log.h"
#include "xdk/hle.h"

namespace hle {
static Prof* g_first = nullptr;
Prof::Prof(const char* n) : name(n), ticks(0), calls(0), next(g_first) { g_first = this; }
}  // namespace hle

uint64_t g_kernel_ticks[400];
uint32_t g_kernel_calls[400];

namespace hle {
void ProfReport(uint32_t frames) {
    if (!frames) frames = 1;
    std::vector<std::pair<uint64_t, Prof*> > top;
    uint64_t all = 0;
    uint32_t all_calls = 0;
    for (Prof* p = g_first; p; p = p->next) {
        if (p->calls) top.push_back(std::make_pair(p->ticks, p));
        all += p->ticks;
        all_calls += p->calls;
    }
    std::sort(top.rbegin(), top.rend());
    const double per = 1.0 / 50000.0 / frames;  // timebase 50 MHz, ms per frame
    std::string line;
    for (size_t i = 0; i < top.size() && i < 24; i++) {
        char b[96];
        _snprintf(b, sizeof(b), " %s %.2f/%u", top[i].second->name + 4, top[i].first * per, top[i].second->calls / frames);
        line += b;
    }
    LOGI("hle: por quadro (%u quadros) %.2f ms em %u chamadas; ms/chamadas:%s", frames, all * per, all_calls / frames,
         line.c_str());
    for (Prof* p = g_first; p; p = p->next) p->ticks = 0, p->calls = 0;
    std::vector<std::pair<uint64_t, int> > kt;
    uint64_t kall = 0;
    for (int i = 0; i < 400; i++)
        if (g_kernel_calls[i]) kt.push_back(std::make_pair(g_kernel_ticks[i], i)), kall += g_kernel_ticks[i];
    std::sort(kt.rbegin(), kt.rend());
    line.clear();
    for (size_t i = 0; i < kt.size() && i < 12; i++) {
        char b[96];
        _snprintf(b, sizeof(b), " %s %.2f/%u", kernel::ExportName(kt[i].second), kt[i].first * per,
                  g_kernel_calls[kt[i].second] / frames);
        line += b;
    }
    LOGI("hle: kernel por quadro %.2f ms (todas as threads):%s", kall * per, line.c_str());
    memset(g_kernel_ticks, 0, sizeof(g_kernel_ticks));
    memset(g_kernel_calls, 0, sizeof(g_kernel_calls));
}
}  // namespace hle
