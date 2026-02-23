#ifndef MOB_AI_OPTIMIZER_OPTIMIZER_H
#define MOB_AI_OPTIMIZER_OPTIMIZER_H

#pragma once

#include <ll/api/mod/NativeMod.h>
#include <ll/api/Config.h>
#include <mc/legacy/ActorUniqueID.h>
#include <unordered_map>
#include <cstdint>

namespace mob_ai_optimizer {

struct Config {
    int version = 1;
    bool enabled = true;
    bool debug = false;

    int cooldownTicks = 300;
    int maxPerTick = 4;
    int cleanupIntervalTicks = 200;
    int maxExpiredAge = 2400;
    std::size_t initialMapReserve = 3000;
};

Config& getConfig();
bool loadConfig();
bool saveConfig();

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

// 主线程独占数据
extern std::unordered_map<ActorUniqueID, std::uint64_t> lastAiTick;
extern int processedThisTick;
extern std::uint64_t lastTickId;
extern int cleanupCounter;

} // namespace mob_ai_optimizer

#endif
