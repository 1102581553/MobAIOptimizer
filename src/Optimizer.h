#pragma once
#include <ll/api/Config.h>
#include <ll/api/io/Logger.h>
#include <ll/api/mod/NativeMod.h>

namespace mob_ai_optimizer {

struct Config {
    int  version       = 4;
    bool enabled       = true;
    int  maxPerTick    = 32;
    int  cooldownTicks = 4;

    // 优先级
    int  reservedSlots      = 8;
    int  priorityAfterTicks = 20;

    // 推挤优化
    bool pushOptEnabled      = true;
    bool disableVec0Push     = true;
    int  maxPushTimesPerTick = 3;    // -1 = 不限制
    bool unlimitedPlayerPush = true;

    // 调试
    bool debug                   = false;
    int  debugLogIntervalSeconds = 5;

    // 内部维护
    int  cleanupIntervalSeconds = 3;
    int  expiryMultiplier       = 2;
};

struct Stats {
    std::uint64_t totalProcessed       = 0;
    std::uint64_t totalCooldownSkipped = 0;
    std::uint64_t totalThrottleSkipped = 0;
    std::uint64_t totalPrioritized     = 0;
};

Config&         getConfig();
ll::io::Logger& logger();

Stats getStats();
void  resetStats();

void registerHooks();
void unregisterHooks();

class PluginImpl {
public:
    static PluginImpl& getInstance();

    PluginImpl() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();

private:
    ll::mod::NativeMod& mSelf;
};

} // namespace mob_ai_optimizer
