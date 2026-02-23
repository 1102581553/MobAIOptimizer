#ifndef MOB_AI_OPTIMIZER_OPTIMIZER_H
#define MOB_AI_OPTIMIZER_OPTIMIZER_H

#pragma once

#include <ll/api/mod/NativeMod.h>
#include <mc/legacy/ActorUniqueID.h>
#include <unordered_map>
#include <cstdint>

namespace mob_ai_optimizer {

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

// 主线程独占数据，无需任何同步原语
extern std::unordered_map<ActorUniqueID, std::uint64_t> lastAiTick;
extern int processedThisTick;
extern std::uint64_t lastTickId;
extern int cleanupCounter;

// ====================== 配置常量 ======================

// AI 冷却时间（tick）- 值越大，生物 AI 执行越稀疏，TPS 越高
constexpr int COOLDOWN_TICKS = 300;

// 每 tick 最大处理生物数 - 控制单帧 CPU 峰值
constexpr int MAX_PER_TICK = 4;

// 清理任务间隔（tick）- 值越小，过期记录清理越及时，内存越稳定
constexpr int CLEANUP_INTERVAL_TICKS = 200;

// 记录最大存活时间（tick）- 控制 unordered_map 最大尺寸
constexpr int MAX_EXPIRED_AGE = 2400;

// 哈希表预分配大小 - 根据服务器生物数量调整，避免频繁 rehash
constexpr std::size_t INITIAL_MAP_RESERVE = 3000;

} // namespace mob_ai_optimizer

#endif // MOB_AI_OPTIMIZER_OPTIMIZER_H
