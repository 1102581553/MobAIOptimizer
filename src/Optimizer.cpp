#include "Optimizer.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/service/Bedrock.h>
#include <ll/api/mod/RegisterHelper.h>
#include <mc/world/actor/Mob.h>
#include <mc/world/level/Level.h>
#include <mc/world/actor/Actor.h>
#include <mc/world/level/Tick.h>  // 必须包含 Tick 定义

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
    &Mob::$aiStep,  // 使用成员指针，而非字符串
    void
) {
    using namespace mob_ai_optimizer;

    auto& level = this->getLevel();
    auto currentTick = level.getCurrentServerTick().tickID;

    // 检测 tick 变化，重置计数器
    if (currentTick != currentTickId) {
        currentTickId = currentTick;
        processedThisTick = 0;
    }

    auto id = this->getUniqueID();

    // 频率限制：每个生物至少间隔 COOLDOWN_TICKS 才执行一次 AI
    auto it = lastAiTick.find(id);
    if (it != lastAiTick.end() && currentTick - it->second < COOLDOWN_TICKS) {
        return;
    }

    // 时间切片：本 tick 已处理的生物数量达到上限，则跳过
    if (processedThisTick >= MAX_PER_TICK) {
        return;
    }

    // 执行原 AI
    processedThisTick++;
    lastAiTick[id] = currentTick;
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
    // 清理该实体的 AI 记录
    mob_ai_optimizer::lastAiTick.erase(this->getUniqueID());
    origin();
}

LL_REGISTER_MOD(mob_ai_optimizer::Optimizer, mob_ai_optimizer::Optimizer::getInstance());
