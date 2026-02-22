#pragma once

#include <ll/api/Config.h>
#include <ll/api/io/Logger.h>
#include <ll/api/mod/NativeMod.h>

namespace mob_ai_optimizer {

struct Config {
    int version = 1;
    bool enabled = true;
    bool debug = false;
    int cooldownTicks = 20;        // AI 冷却时间（tick）
    int maxPerTick = 50;            // 每 tick 最多处理的生物数
    int cleanupInterval = 1000;     // 过期清理间隔（tick）
    int maxExpiredAge = 10000;      // 过期阈值（tick）
};

Config& getConfig();
bool loadConfig();
bool saveConfig();

void clearCache();
ll::io::Logger& logger();

class Optimizer {
public:
    static Optimizer& getInstance();

    Optimizer() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();

private:
    ll::mod::NativeMod& mSelf;
};

} // namespace mob_ai_optimizer
