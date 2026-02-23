#ifndef MOB_AI_OPTIMIZER_OPTIMIZER_H
#define MOB_AI_OPTIMIZER_OPTIMIZER_H

#pragma once

#include <ll/api/mod/NativeMod.h>
#include <mc/legacy/ActorUniqueID.h>
#include <unordered_map>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>
#include <future>

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

// 全局数据声明
extern std::unordered_map<ActorUniqueID, std::uint64_t> lastAiTick;
extern std::mutex lastAiTickMutex;
extern std::atomic<int> processedThisTick;
extern std::atomic<std::uint64_t> currentTickId;
extern std::atomic<int> cleanupCounter;

// 异步任务管理
extern std::vector<std::future<void>> cleanupTasks;
extern std::mutex cleanupTasksMutex;
extern std::atomic<bool> stopping;

// 配置常量
constexpr int COOLDOWN_TICKS = 100;
constexpr int MAX_PER_TICK   = 1;

constexpr std::size_t INITIAL_MAP_RESERVE = 10000;
constexpr int CLEANUP_INTERVAL_TICKS = 1000;
constexpr int MAX_EXPIRED_AGE = 10000;

} // namespace mob_ai_optimizer

#endif // MOB_AI_OPTIMIZER_OPTIMIZER_H
