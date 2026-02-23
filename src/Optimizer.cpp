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

// 定义全局变量（与头文件声明一致）
std::unordered_map<ActorUniqueID, std::uint64_t> lastAiTick;
std::mutex lastAiTickMutex;
std::atomic<int> processedThisTick{0};
std::atomic<std::uint64_t> currentTickId{0};
int cleanupCounter = 0;

Optimizer& Optimizer::getInstance() {
    static Optimizer instance;
    return instance;
}

bool Optimizer::load() {
    getSelf().getLogger().info("生物AI优化插件已加载");
    {
        std::lock_guard<std::mutex> lock(lastAiTickMutex);
        lastAiTick.reserve(INITIAL_MAP_RESERVE); // 使用头文件常量
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
        lastAiTick.clear();
    }
    return true;
}

void performCleanup(std::uint64_t currentTick) {
    std::lock_guard<std::mutex> lock(lastAiTickMutex);
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
    &Mob::$aiStep,
    void
) {
    using namespace mob_ai_optimizer;

    // 直接获取当前 tick（getLevel() 返回引用，无需检查）
    std::uint64_t currentTick = this->getLevel().getCurrentServerTick().tickID;

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
            return; // 还在冷却期，跳过本次 AI（不调用 origin）
        }
    }

    int processed = processedThisTick.load();
    if (processed >= MAX_PER_TICK) {
        return; // 本 tick 已达上限，跳过
    }

    // 尝试原子递增，防止并发超限
    if (!processedThisTick.compare_exchange_strong(processed, processed + 1)) {
        if (processed >= MAX_PER_TICK) return;
        processedThisTick.fetch_add(1);
    }

    // 执行原始 AI
    origin();

    // AI 成功执行后，更新最后执行 tick
    {
        std::lock_guard<std::mutex> lock(lastAiTickMutex);
        lastAiTick[id] = currentTick;
    }
}

// ====================== 自动清理 Hook ======================
LL_AUTO_TYPE_INSTANCE_HOOK(
    ActorDespawnHook,
    ll::memory::HookPriority::Normal,
    Actor,
    &Actor::$despawn,
    void
) {
    using namespace mob_ai_optimizer;
    {
        std::lock_guard<std::mutex> lock(lastAiTickMutex);
        lastAiTick.erase(this->getOrCreateUniqueID());
    }
    origin();
}

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
