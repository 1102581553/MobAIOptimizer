#include "Optimizer.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/service/Bedrock.h>
#include <ll/api/mod/RegisterHelper.h>
#include <ll/api/event/EventBus.h>
#include <ll/api/event/actor/ActorRemovedEvent.h>   // 根据实际路径调整
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
    // 注册 ActorRemovedEvent 监听器，清理已移除实体的缓存
    auto& eventBus = ll::event::EventBus::getInstance();
    mListener = eventBus.emplaceListener<ll::event::actor::ActorRemovedEvent>(
        [](ll::event::actor::ActorRemovedEvent& ev) {
            // 从事件中获取被移除的实体指针（API 可能为 getActor() 或 self()）
            if (auto actor = ev.getActor()) {          // 假设事件提供 getActor()
                lastAiTick.erase(actor->getOrCreateUniqueID());
            }
        }
    );

    getSelf().getLogger().info("生物AI优化插件已启用");
    return true;
}

bool Optimizer::disable() {
    // 取消事件监听
    if (mListener) {
        auto& eventBus = ll::event::EventBus::getInstance();
        eventBus.removeListener(mListener);
        mListener = nullptr;
    }
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

    Mob* self = (Mob*)this;                           // 转换为 Mob 指针
    auto& level = self->getLevel();
    auto currentTick = level.getCurrentServerTick().tickID;
    int tickInt = static_cast<int>(currentTick);

    if (tickInt != currentTickId) {
        currentTickId = tickInt;
        processedThisTick = 0;
    }

    ActorUniqueID id = self->getOrCreateUniqueID();   // 正确获取唯一ID

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

LL_REGISTER_MOD(mob_ai_optimizer::Optimizer, mob_ai_optimizer::Optimizer::getInstance());
