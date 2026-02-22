#pragma once

#include <ll/api/mod/NativeMod.h>
#include <ll/api/Logger.h>
#include <mc/legacy/ActorUniqueID.h>
#include <unordered_map>

namespace mob_ai_optimizer {

struct Config {
    int cooldownTicks = 5;   // 每个生物至少间隔多少 tick 执行AI
    int maxPerTick    = 50;  // 每 tick 最多处理多少个生物
    bool debug        = false; // 开启后输出详细优化日志
};

class Optimizer {
public:
    static Optimizer& getInstance();

    Optimizer() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }
    [[nodiscard]] ll::Logger& getLogger() { return mSelf.getLogger(); }

    bool load();
    bool enable();
    bool disable();

    void loadConfig();   // 加载/生成配置文件

private:
    ll::mod::NativeMod& mSelf;
};

// 全局数据
extern Config config;
extern std::unordered_map<ActorUniqueID, int> lastAiTick;
extern int processedThisTick;
extern int skippedThisTick;
extern int currentTickId;

} // namespace mob_ai_optimizer
