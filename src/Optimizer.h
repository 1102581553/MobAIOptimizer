#pragma once
#include <ll/api/Config.h>
#include <ll/api/io/Logger.h>
#include <ll/api/mod/NativeMod.h>
#include <unordered_map>
#include <mc/legacy/ActorUniqueID.h>

namespace mob_ai_optimizer {

struct Config {
    int  version = 4;                // 配置版本（自增以标识变更）
    bool enabled = true;              // 总开关
    bool debug   = false;             // 调试输出开关

    // AI 处理间隔（冷却 ticks）
    int aiCooldownTicks = 4;           // 每个生物两次 AI 之间的最小间隔 tick 数

    // 内部优化参数
    int initialMapReserve = 1000;      // 冷却表预分配大小
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
