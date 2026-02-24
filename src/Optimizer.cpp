#include "Optimizer.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/io/LoggerRegistry.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"
#include "mc/entity/components_json_legacy/PushableComponent.h"
#include "mc/deps/core/math/Vec3.h"

#include <filesystem>
#include <unordered_map>

namespace mob_ai_optimizer {

// ── 每个实体的状态 ────────────────────────────────────────
struct ActorState {
    std::uint64_t lastAiTick   = 0;
    std::uint64_t pendingSince = 0;
};

// ── 全局状态 ──────────────────────────────────────────────
static Config                          config;
static std::shared_ptr<ll::io::Logger> log;
static Stats                           gStats{};

static std::uint64_t lastTickId        = 0;
static int           processedThisTick = 0;

static std::uint64_t lastDebugTick   = 0;
static std::uint64_t lastCleanupTick = 0;

static std::unordered_map<std::int64_t, ActorState> actorStates;
static std::unordered_map<Actor*, int>               pushedEntityTimes;

// ─────────────────────────────────────────────────────────

Config& getConfig() { return config; }

ll::io::Logger& logger() {
    if (!log) {
        log = ll::io::LoggerRegistry::getInstance().getOrCreate("MobAIOptimizer");
    }
    return *log;
}

Stats getStats()   { return gStats; }
void  resetStats() { gStats = {}; }

// ── ActorTick Hook ────────────────────────────────────────
LL_TYPE_INSTANCE_HOOK(
    ActorTickHook,
    ll::memory::HookPriority::Normal,
    Actor,
    &Actor::tick,
    bool,
    ::BlockSource& region
) {
    if (!config.enabled || this->isPlayer()) {
        return origin(region);
    }

    const std::uint64_t currentTick =
        this->getLevel().getCurrentTick().tickID;

    // ── Tick 切换 ─────────────────────────────────────────
    if (currentTick != lastTickId) {
        lastTickId        = currentTick;
        processedThisTick = 0;

        // 定时清理过期状态
        const std::uint64_t cleanupInterval =
            static_cast<std::uint64_t>(config.cleanupIntervalSeconds) * 20;

        if (currentTick - lastCleanupTick >= cleanupInterval) {
            lastCleanupTick = currentTick;

            const std::uint64_t expiry =
                static_cast<std::uint64_t>(config.cooldownTicks) *
                static_cast<std::uint64_t>(config.expiryMultiplier);

            for (auto it = actorStates.begin(); it != actorStates.end();) {
                if (currentTick - it->second.lastAiTick > expiry)
                    it = actorStates.erase(it);
                else
                    ++it;
            }
        }

        // Debug 日志
        if (config.debug) {
            const std::uint64_t debugInterval =
                static_cast<std::uint64_t>(config.debugLogIntervalSeconds) * 20;

            if (currentTick - lastDebugTick >= debugInterval) {
                lastDebugTick = currentTick;

                logger().info(
                    "[Debug] processed={}, cooldownSkipped={}, throttleSkipped={}, prioritized={}, cacheSize={}",
                    gStats.totalProcessed,
                    gStats.totalCooldownSkipped,
                    gStats.totalThrottleSkipped,
                    gStats.totalPrioritized,
                    actorStates.size()
                );
            }
        }
    }

    const std::int64_t uid = this->getOrCreateUniqueID().rawID;
    auto [it, inserted]    = actorStates.emplace(uid, ActorState{});

    ActorState& state = it->second;

    // ── 冷却判断 ──────────────────────────────────────────
    if (!inserted &&
        currentTick - state.lastAiTick <
            static_cast<std::uint64_t>(config.cooldownTicks))
    {
        ++gStats.totalCooldownSkipped;
        return true;
    }

    // ── 判断是否进入优先队列 ──────────────────────────────
    const bool isWaiting     = state.pendingSince > 0;
    const bool isPrioritized =
        isWaiting &&
        config.priorityAfterTicks > 0 &&
        (currentTick - state.pendingSince >=
            static_cast<std::uint64_t>(config.priorityAfterTicks));

    // ── 限流 ──────────────────────────────────────────────
    const int normalLimit    = config.maxPerTick - config.reservedSlots;
    const int effectiveLimit = isPrioritized ? config.maxPerTick : normalLimit;

    if (processedThisTick >= effectiveLimit) {
        if (!isWaiting) {
            state.pendingSince = currentTick;
        }
        ++gStats.totalThrottleSkipped;
        return true;
    }

    // ── 实际处理 ──────────────────────────────────────────
    ++processedThisTick;

    if (isPrioritized) {
        ++gStats.totalPrioritized;
    }

    state.pendingSince = 0;
    state.lastAiTick   = currentTick;

    bool result = origin(region);
    ++gStats.totalProcessed;
    return result;
}

// ── Push Hook：跳过零向量 ─────────────────────────────────
LL_TYPE_INSTANCE_HOOK(
    PushVec0Hook,
    ll::memory::HookPriority::Normal,
    PushableComponent,
    &PushableComponent::push,
    void,
    Actor&      owner,
    Vec3 const& vec
) {
    if (!config.pushOptEnabled || !config.disableVec0Push) {
        return origin(owner, vec);
    }
    if (vec == Vec3::ZERO) {
        return;
    }
    origin(owner, vec);
}

// ── Push Hook：限制每实体每 tick 被推次数 ─────────────────
LL_TYPE_INSTANCE_HOOK(
    PushMaxTimesHook,
    ll::memory::HookPriority::Normal,
    PushableComponent,
    &PushableComponent::push,
    void,
    Actor& owner,
    Actor& other,
    bool   pushSelfOnly
) {
    if (!config.pushOptEnabled || config.maxPushTimesPerTick < 0) {
        return origin(owner, other, pushSelfOnly);
    }
    if (config.unlimitedPlayerPush && (owner.isPlayer() || other.isPlayer())) {
        return origin(owner, other, pushSelfOnly);
    }

    auto it = pushedEntityTimes.find(&owner);
    if (it != pushedEntityTimes.end()) {
        if (it->second >= config.maxPushTimesPerTick) {
            return;
        }
        ++it->second;
    } else {
        pushedEntityTimes[&owner] = 1;
    }

    origin(owner, other, pushSelfOnly);
}

// ── Level::tick Hook：每 tick 清理推挤计数 ────────────────
LL_TYPE_INSTANCE_HOOK(
    LevelTickHook,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::$tick,
    void
) {
    origin();
    pushedEntityTimes.clear();
}

// ── 注册 / 注销 ───────────────────────────────────────────
void registerHooks() {
    ActorTickHook::hook();
    PushVec0Hook::hook();
    PushMaxTimesHook::hook();
    LevelTickHook::hook();
}

void unregisterHooks() {
    ActorTickHook::unhook();
    PushVec0Hook::unhook();
    PushMaxTimesHook::unhook();
    LevelTickHook::unhook();
}

// ── PluginImpl ────────────────────────────────────────────
PluginImpl& PluginImpl::getInstance() {
    static PluginImpl instance;
    return instance;
}

bool PluginImpl::load() {
    std::filesystem::create_direct_
