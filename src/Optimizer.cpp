#include "Optimizer.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/service/Bedrock.h>
#include <ll/api/mod/RegisterHelper.h>
#include <mc/world/actor/Mob.h>
#include <mc/world/actor/Actor.h>
#include <mc/world/level/Level.h>
#include <mc/world/level/Tick.h>
#include <mc/legacy/ActorUniqueID.h>

namespace mob_ai_optimizer {

std::unordered_map<ActorUniqueID, int> lastAiTick;
int processedThisTick = 0;
int currentTickId = -1;

Optimizer& Optimizer::getInstance() {
    static Optimizer instance;
    return instance;
}

bool Optimizer::load() {
    getSelf().getLogger().info("生物AI优化插件已加载");
    return true;
}

bool Optimizer::enable() {
    getSelf().getLogger().info("生物AI优化插件已启用");
    return true;
}

bool Optimizer::disable() {
    getSelf().getLogger().info("生物AI优化插件已禁用");
    return true;
}

} // namespace mob_ai_optimizer

// Hook Mob::aiStep 的 thunk 函数
LL_AUTO_TYPE_INSTANCE_HOOK(
    MobAiStepHook,
    ll::memory::HookPriority::Normal,
    Mob,
    &Mob::$aiStep,
    void
) {
    using namespace mob_ai_optimizer;

    auto& level = this->getLevel();
    auto currentTick = level.getCurrentServerTick().tickID;
    int tickInt = static_cast<int>(currentTick);

    if (tickInt != currentTickId) {
        currentTickId = tickInt;
        processedThisTick = 0;
    }

    ActorUniqueID id = this->getUniqueID();

    auto it = lastAiTick.find(id);
    if (it != lastAiTick.end() && tickInt - it->second < COOLDOWN_TICKS) {
        return;
    }

    if (processedThisTick >= MAX_PER_TICK) {
        return;
    }

    processedThisTick++;
    lastAiTick[id] = tickInt;
    origin();
}

// Hook Actor::remove 的 thunk 函数，用于清理缓存
LL_AUTO_TYPE_INSTANCE_HOOK(
    ActorRemoveHook,
    ll::memory::HookPriority::Normal,
    Actor,
    &Actor::$remove,
    void
) {
    mob_ai_optimizer::lastAiTick.erase(this->getUniqueID());
    origin();
}

LL_REGISTER_MOD(mob_ai_optimizer::Optimizer, mob_ai_optimizer::Optimizer::getInstance());
