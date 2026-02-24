// Optimizer.h
#pragma once
#include <ll/api/Config.h>
#include <ll/api/io/Logger.h>
#include <ll/api/mod/NativeMod.h>

namespace mob_ai_optimizer {

struct Config {
    int  version       = 1;
    bool enabled       = true;
    int  maxPerTick    = 32;
    int  cooldownTicks = 4;
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
