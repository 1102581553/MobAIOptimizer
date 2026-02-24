#pragma once
#include <ll/api/Config.h>
#include <ll/api/io/Logger.h>
#include <ll/api/mod/NativeMod.h>
#include <unordered_map>
#include <mc/legacy/ActorUniqueID.h>

namespace mob_ai_optimizer {

struct Config {
    int  version = 2;
    bool enabled = true;
    bool debug   = false;

    // 动态调节目标
    int targetTickMs      = 50;
    int maxPerTickStep    = 4;
    int cooldownTicksStep = 1;

    // 内部维护
    int cleanupIntervalTicks = 100;
    int maxExpiredAge        = 600;
    int initialMapReserve    = 1000;
};

Config& getConfig();
bool    loadConfig();
bool    saveConfig();

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
