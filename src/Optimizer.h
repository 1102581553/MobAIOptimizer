#pragma once
#include <cstdint>
#include <unordered_map>
#include "ll/api/Config.h"

namespace mob_ai_optimizer {

// ── 配置 ──────────────────────────────────────────────────────────────────────
struct Config {
    int  version       = 1;
    bool enabled       = true;
    int  maxPerTick    = 200;
    int  cooldownTicks = 3;
};

// ── 统计 ──────────────────────────────────────────────────────────────────────
struct Stats {
    std::uint64_t totalProcessed       = 0;
    std::uint64_t totalCooldownSkipped = 0;
    std::uint64_t totalThrottleSkipped = 0;
};

Stats getStats();
void  resetStats();

void registerHooks();
void unregisterHooks();

} // namespace mob_ai_optimizer
