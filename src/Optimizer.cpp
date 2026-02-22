#include "Optimizer.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/mod/RegisterHelper.h>
#include <mc/world/actor/Mob.h>
#include <mc/world/actor/Actor.h>
#include <mc/world/level/Level.h>
#include <mc/world/level/Tick.h>
#include <mc/legacy/ActorUniqueID.h>

namespace mob_ai_optimizer {

// ============================ 配置项（可自行调整） ============================
constexpr size_t INITIAL_MAP_RESERVE   = 12000;   // 预分配 map 大小，避免频繁 rehash
constexpr int   CLEANUP_INTERVAL_TICKS = 1200;    // 每 1200 ticks 执行一次过期清理
constexpr int   MAX_EXPIRED_AGE        = 12000;   // 超过此 tick 数未更新的条目视为过期

// 全局变量（在 Optimizer.h 中用 extern 声明）
std::unordered_map<ActorUniqueID, int> lastAiTick;
int processedThisTick = 0;
int currentTickId     = -1;
int cleanupCounter    = 0;

Optimizer& Optimizer::getInstance() {
    static Optimizer instance;
    return instance;
}

bool Optimizer::load() {
    getSelf().getLogger().info("§a生物AI优化插件已加载");
    lastAiTick.reserve(INITIAL_MAP_RESERVE);
    return true;
}

bool Optimizer::enable() {
    getSelf().getLogger().info("§a生物AI优化插件已启用");
    return true;
}

bool Optimizer::disable() {
    getSelf().getLogger().info("§c生物AI优化插件已禁用");
    lastAiTick.clear();
    return true;
}

// 定期清理过期条目（防止内存泄漏）
void performCleanup(int currentTick) {
    for (auto it = lastAiTick.begin(); it != lastAiTick.end(); ) {
        if (currentTick - it->second > MAX_EXPIRED_AGE) {
            it = lastAiTick.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace mob_ai_optimizer

// ====================== AI优化核心 Hook ======================
LL_AUTO_TYPE_INSTANCE_HOOK(
    MobAiStepHook,
    ll::memory::HookPriority::Normal,
    Mob,
    &Mob::$aiStep,
    void
) {
    using namespace mob_ai_optimizer;

    auto& level  = this->getLevel();
    int   tickInt = static_cast<int>(level.getCurrentServerTick().tickID);

    // Tick 切换时重置计数器和执行清理
    if (tickInt != currentTickId) {
        currentTickId     = tickInt;
        processedThisTick = 0;
        cleanupCounter++;

        if (cleanupCounter >= CLEANUP_INTERVAL_TICKS) {
            performCleanup(tickInt);
            cleanupCounter = 0;
        }
    }

    ActorUniqueID id = this->getOrCreateUniqueID();

    // 冷却期跳过
    auto it = lastAiTick.find(id);
    if (it != lastAiTick.end() && tickInt - it->second < COOLDOWN_TICKS) {
        return;
    }

    // 本 tick 处理上限
    if (processedThisTick >= MAX_PER_TICK) {
        return;
    }

    processedThisTick++;
    lastAiTick[id] = tickInt;   // 记录本次处理时间

    origin(); // 执行原始 aiStep
}

// ====================== 实体移除清理 Hook ======================
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
