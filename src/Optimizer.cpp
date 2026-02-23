#include "Optimizer.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/mod/RegisterHelper.h>
#include <ll/api/io/Logger.h>
#include <ll/api/io/LoggerRegistry.h>
#include <mc/world/actor/Mob.h>
#include <mc/world/actor/Actor.h>
#include <mc/world/level/Level.h>
#include <mc/world/level/Tick.h>
#include <mc/legacy/ActorUniqueID.h>

namespace mob_ai_optimizer {

// ====================== 全局变量定义 ======================
std::unordered_map<ActorUniqueID, std::uint64_t> lastAiTick;
int processedThisTick = 0;
std::uint64_t lastTickId = 0;
int cleanupCounter = 0;

// ====================== Logger ======================
ll::io::Logger& getLogger() {
    static auto instance = ll::io::LoggerRegistry::getInstance()
                              .getOrCreate("MobAIOptimizer");
    return *instance;
}

// ====================== 插件生命周期 ======================
Optimizer& Optimizer::getInstance() {
    static Optimizer instance;
    return instance;
}

bool Optimizer::load() {
    getLogger().info("生物AI优化插件已加载");
    lastAiTick.reserve(INITIAL_MAP_RESERVE);
    return true;
}

bool Optimizer::enable() {
    getLogger().info("生物AI优化插件已启用");
    return true;
}

bool Optimizer::disable() {
    getLogger().info("生物AI优化插件已禁用");
    lastAiTick.clear();
    processedThisTick = 0;
    lastTickId = 0;
    cleanupCounter = 0;
    return true;
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

    std::uint64_t currentTick = this->getLevel().getCurrentServerTick().tickID;

    // tick 重置：新的一 tick 开始时重置计数器，并按间隔执行清理
    if (currentTick != lastTickId) {
        lastTickId = currentTick;
        processedThisTick = 0;

        if (++cleanupCounter >= CLEANUP_INTERVAL_TICKS) {
            cleanupCounter = 0;
            for (auto it = lastAiTick.begin(); it != lastAiTick.end(); ) {
                if (currentTick - it->second > MAX_EXPIRED_AGE)
                    it = lastAiTick.erase(it);
                else
                    ++it;
            }
        }
    }

    // 每 tick 限流：达到上限后直接跳过，连哈希查找都不做
    if (processedThisTick >= MAX_PER_TICK) return;

    // 冷却检查 + 写回合并为一次哈希查找
    auto [it, inserted] = lastAiTick.emplace(this->getOrCreateUniqueID(), 0);
    if (!inserted && currentTick - it->second < COOLDOWN_TICKS) return;

    ++processedThisTick;
    origin();
    it->second = currentTick;
}

// ====================== 自动清理 Hook ======================
LL_AUTO_TYPE_INSTANCE_HOOK(
    ActorDespawnHook,
    ll::memory::HookPriority::Normal,
    Actor,
    &Actor::$despawn,
    void
) {
    mob_ai_optimizer::lastAiTick.erase(this->getOrCreateUniqueID());
    origin();
}

LL_AUTO_TYPE_INSTANCE_HOOK(
    ActorRemoveHook,
    ll::memory::HookPriority::Normal,
    Actor,
    &Actor::$remove,
    void
) {
    mob_ai_optimizer::lastAiTick.erase(this->getOrCreateUniqueID());
    origin();
}

// ====================== 注册插件 ======================
LL_REGISTER_MOD(mob_ai_optimizer::Optimizer, mob_ai_optimizer::Optimizer::getInstance());
