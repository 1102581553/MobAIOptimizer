// Optimizer.h
#pragma once
#include <ll/api/Config.h>
#include <ll/api/io/Logger.h>
#include <ll/api/mod/NativeMod.h>

namespace mob_ai_optimizer {

struct Config {
    int  version       = 2;

    bool enabled       = true;

    int  maxPerTick    = 32;
    int  cooldownTicks = 4;

    // ───── 新增 ─────
    bool debug                     = false; // 是否开启调试日志
    int  debugLogIntervalSeconds   = 5;     // 每多少秒打印一次统计
    int  cleanupIntervalSeconds    = 3;     // 多久清理一次 UID 表
    int  expiryMultiplier          = 2;     // UID 过期倍数
};

struct Stats {
    std::uint64_t totalProcessed       = 0;
    std::uint64_t totalCooldownSkipped = 0;
    std::uint64_t totalThrottleSkipped = 0;
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
