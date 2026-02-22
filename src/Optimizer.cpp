#include "Optimizer.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/mod/RegisterHelper.h>
#include <mc/world/actor/Mob.h>
#include <mc/world/actor/Actor.h>
#include <mc/world/level/Level.h>
#include <mc/world/level/Tick.h>
#include <mc/legacy/ActorUniqueID.h>

namespace mob_ai_optimizer {

// 配置常量（可调整）
constexpr size_t INITIAL_MAP_RESERVE = 10000; // 预分配大小，基于预期Mob数量
constexpr int CLEANUP_INTERVAL_TICKS = 1000; // 每1000 ticks清理一次过期条目
constexpr int MAX_EXPIRED_AGE = 10000; // 过期阈值：如果lastAiTick距今>此值，视为过期

std::unordered_map<ActorUniqueID, int> lastAiTick;
int processedThisTick = 0;
int currentTickId = -1;
int cleanupCounter = 0; // 清理计数器

Optimizer& Optimizer::getInstance() {
    static Optimizer instance;
    return instance;
}

bool Optimizer::load() {
    getSelf().getLogger().info("生物AI优化插件已加载");
    lastAiTick.reserve(INITIAL_MAP_RESERVE); // 预分配减少rehash
    return true;
}

bool Optimizer::enable() {
    getSelf().getLogger().info("生物AI优化插件已启用");
    return true;
}

bool Optimizer::disable() {
    getSelf().getLogger().info("生物AI优化插件已禁用");
    lastAiTick.clear(); // 禁用时清理map
    return true;
}

void performCleanup(int currentTick) {
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
    auto* self = this;
    auto& level = self->getLevel();
    auto currentTick = level.getCurrentServerTick().tickID;
    int tickInt = static_cast<int>(currentTick);

    if (tickInt != currentTickId) {
        currentTickId = tickInt;
        processedThisTick = 0;
        cleanupCounter++;
        if (cleanupCounter >= CLEANUP_INTERVAL_TICKS) {
            performCleanup(tickInt);
            cleanupCounter = 0;
        }
    }

    ActorUniqueID id = self->getOrCreateUniqueID();
    auto it = lastAiTick.find(id);
    if (it != lastAiTick.end() && tickInt - it->second < COOLDOWN_TICKS) {
        return; // 还在冷却期，跳过本次AI
    }
    if (processedThisTick >= MAX_PER_TICK) {
        return; // 本tick已达上限
    }
    processedThisTick++;

    lastAiTick[id] = tickInt;
    origin(); // 执行原始AI
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
    lastAiTick.erase(this->getOrCreateUniqueID());
    origin();
}

// ====================== 额外钩子：Actor移除钩子 ======================
// 钩住Actor::remove 以捕获更多移除场景（e.g., kill, explosion）
LL_AUTO_TYPE_INSTANCE_HOOK(
    ActorRemoveHook,
    ll::memory::HookPriority::Normal,
    Actor,
    &Actor::$remove,
    void
) {
    using namespace mob_ai_optimizer;
    lastAiTick.erase(this->getOrCreateUniqueID());
    origin();
}

LL_REGISTER_MOD(mob_ai_optimizer::Optimizer, mob_ai_optimizer::Optimizer::getInstance());
