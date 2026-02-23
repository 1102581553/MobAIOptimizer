#pragma once

#include <ll/api/mod/NativeMod.h>
#include <mc/legacy/ActorUniqueID.h>
#include <unordered_map>
#include <atomic>      // 添加原子类型支持
#include <cstdint>     // 添加 uint64_t

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

// 全局数据声明（与源文件定义类型严格一致）
extern std::unordered_map<ActorUniqueID, std::uint64_t> lastAiTick;
extern std::atomic<int> processedThisTick;
extern std::atomic<std::uint64_t> currentTickId;

// 配置常量（统一在头文件中定义）
constexpr int COOLDOWN_TICKS = 100;   // 每个生物至少间隔100 tick执行AI（根据头文件原值）
constexpr int MAX_PER_TICK   = 1;     // 每tick最多处理1个生物（根据头文件原值）

// 其他常量（若需要也可放在此处）
constexpr std::size_t INITIAL_MAP_RESERVE = 10000;
constexpr int CLEANUP_INTERVAL_TICKS = 1000;
constexpr int MAX_EXPIRED_AGE = 10000;

} // namespace mob_ai_optimizer
