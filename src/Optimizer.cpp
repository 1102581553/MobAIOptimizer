#include "Optimizer.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/mod/RegisterHelper.h>
#include <mc/world/actor/Mob.h>
#include <mc/world/actor/Actor.h>
#include <mc/world/level/Level.h>
#include <mc/world/level/Tick.h>
#include <mc/legacy/ActorUniqueID.h>
#include <unordered_map>
#include <mutex>
#include <atomic>

namespace mob_ai_optimizer {

// 配置常量（若 Optimizer.h 未定义，则在此定义默认值）
#ifndef COOLDOWN_TICKS
#define COOLDOWN_TICKS 20        // 两次AI执行的最小间隔 tick 数
#endif
#ifndef MAX_PER_TICK
#define MAX_PER_TICK 10          // 每 tick 最多处理的生物数
#endif
#ifndef INITIAL_MAP_RESERVE
#define INITIAL_MAP_RESERVE 10000 // 预分配大小
#endif
#ifndef CLEANUP_INTERVAL_TICKS
#define CLEANUP_INTERVAL_TICKS 1000 // 每 1000 ticks 清理过期条目
#endif
#ifndef MAX_EXPIRED_AGE
#define MAX_EXPIRED_AGE 10000     // 过期阈值
#endif

std::unordered_map<ActorUniqueID, uint64_t> lastAiTick; // 改为 uint64_t 存储 tick
std::mutex lastAiTickMutex;                              // 保护 lastAiTick 的互斥锁
std::atomic<int> processedThisTick{0};                   // 本 tick 已处理计数
std::atomic<uint64_t> currentTickId{0};                  // 当前 tick ID
int cleanupCounter = 0;                                   // 清理计数器

Optimizer& Optimizer::getInstance() {
    static Optimizer instance;
    return instance;
}

bool Optimizer::load() {
    getSelf().getLogger().info("生物AI优化插件已加载");
    {
        std::lock_guard<std::mutex> lock(lastAiTickMutex);
        lastAiTick.reserve(INITIAL_MAP_RESERVE); // 预分配减少 rehash
    }
    return true;
}

bool Optimizer::enable() {
    getSelf().getLogger().info("生物AI优化插件已启用");
    return true;
}

bool Optimizer::disable() {
    getSelf().getLogger().info("生物AI优化插件已禁用");
    {
        std::lock_guard<std::mutex> lock(lastAiTickMutex);
        lastAiTick.clear(); // 禁用时清理 map
    }
    return true;
}

void performCleanup(uint64_t currentTick) {
    std::lock_guard<std::mutex> lock(lastAiTickMutex);
    // 清理过期条目，防止泄漏
    for (auto it = lastAiTick.begin(); it != lastAiTick.end(); ) {
        if (currentTick - it->second > MAX_EXPIRED_AGE) {
            it = lastAiTick.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace mob_ai_optimizer

// ====================== AI优化 Hook ======================
LL_AUTO_TYPE_INSTANCE_HOOK(
    MobAiStepHook,
    ll::memory::HookPriority::Normal,
    Mob,
    &Mob::$aiStep, // 必须加 $，因为 aiStep 是虚函数
    void
) {
    using namespace mob_ai_optimizer;

    // 获取当前 tick ID（安全转换）
    auto& level = this->getLevel();
    if (!level.has_value()) { // 检查 level 是否有效
        origin();
        return;
    }
    uint64_t currentTick = level->getCurrentServerTick().tickID;

    // 跨 tick 重置计数
    if (currentTick != currentTickId.load()) {
        currentTickId.store(currentTick);
        processedThisTick.store(0);
        cleanupCounter++;
        if (cleanupCounter >= CLEANUP_INTERVAL_TICKS) {
            performCleanup(currentTick);
            cleanupCounter = 0;
        }
    }

    // 检查冷却与每 tick 上限
    ActorUniqueID id = this->getOrCreateUniqueID();
    {
        std::lock_guard<std::mutex> lock(lastAiTickMutex);
        auto it = lastAiTick.find(id);
        if (it != lastAiTick.end() && currentTick - it->second < COOLDOWN_TICKS) {
            return; // 还在冷却期，跳过本次 AI
        }
    }

    int processed = processedThisTick.load();
    if (processed >= MAX_PER_TICK) {
        return; // 本 tick 已达上限
    }

    // 尝试原子递增，防止并发超限
    if (!processedThisTick.compare_exchange_strong(processed, processed + 1)) {
        // 如果交换失败，说明其他线程已修改，重新判断
        if (processed >= MAX_PER_TICK) return;
        processedThisTick.fetch_add(1);
    }

    // 执行原始 AI（可能抛出异常，但异常不可恢复，故不额外处理）
    origin();

    // AI 成功执行后，更新最后执行 tick
    {
        std::lock_guard<std::mutex> lock(lastAiTickMutex);
        lastAiTick[id] = currentTick;
    }
}

// ====================== 自动清理 Hook ======================
// 生物/实体被移除（despawn）时自动删除缓存，防止内存泄漏
LL_AUTO_TYPE_INSTANCE_HOOK(
    ActorDespawnHook,
    ll::memory::HookPriority::Normal,
    Actor,
    &Actor::$despawn,
    void
) {
    using namespace mob_ai_optimizer;
    // 先删除缓存，再调用原函数，确保无论原函数成功与否，缓存都被清理
    {
        std::lock_guard<std::mutex> lock(lastAiTickMutex);
        lastAiTick.erase(this->getOrCreateUniqueID());
    }
    origin();
}

// ====================== 额外钩子：Actor移除钩子 ======================
// 钩住 Actor::remove 以捕获更多移除场景（e.g., kill, explosion）
LL_AUTO_TYPE_INSTANCE_HOOK(
    ActorRemoveHook,
    ll::memory::HookPriority::Normal,
    Actor,
    &Actor::$remove,
    void
) {
    using namespace mob_ai_optimizer;
    {
        std::lock_guard<std::mutex> lock(lastAiTickMutex);
        lastAiTick.erase(this->getOrCreateUniqueID());
    }
    origin();
}

LL_REGISTER_MOD(mob_ai_optimizer::Optimizer, mob_ai_optimizer::Optimizer::getInstance());
