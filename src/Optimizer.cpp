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
static std::unordered_map<Actor*, int>              pushedEntityTimes;

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

    if (currentTick != lastTickId) {
        lastTickId        = currentTick;
        processedThisTick = 0;

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

    if (!inserted &&
        currentTick - state.lastAiTick <
            static_cast<std::uint64_t>(config.cooldownTicks))
    {
        ++gStats.totalCooldownSkipped;
        return true;
    }

    const bool isWaiting     = state.pendingSince > 0;
    const bool isPrioritized =
        isWaiting &&
        config.priorityAfterTicks > 0 &&
        (currentTick - state.pendingSince >=
            static_cast<std::uint64_t>(config.priorityAfterTicks));

    const int normalLimit    = config.maxPerTick - config.reservedSlots;
    const int effectiveLimit = isPrioritized ? config.maxPerTick : normalLimit;

    if (processedThisTick >= effectiveLimit) {
        if (!isWaiting) {
            state.pendingSince = currentTick;
        }
        ++gStats.totalThrottleSkipped;
        return true;
    }

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

    // 修正：ZERO 是函数
    if (vec == Vec3::ZERO()) {
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
    std::filesystem::create_directories(getSelf().getConfigDir());
    const auto configPath = getSelf().getConfigDir() / "config.json";

    if (!ll::config::loadConfig(config, configPath)) {
        logger().warn("Failed to load config, using defaults.");
        ll::config::saveConfig(config, configPath);
    }

    return true;
}

bool PluginImpl::enable() {
    if (config.reservedSlots >= config.maxPerTick) {
        logger().warn(
            "reservedSlots({}) >= maxPerTick({}), resetting to half.",
            config.reservedSlots,
            config.maxPerTick
        );
        config.reservedSlots = config.maxPerTick / 2;
    }

    registerHooks();

    logger().info(
        "Enabled. maxPerTick={}, cooldownTicks={}, reservedSlots={}, priorityAfterTicks={}, "
        "pushOpt={}, disableVec0Push={}, maxPushTimesPerTick={}, debug={}",
        config.maxPerTick,
        config.cooldownTicks,
        config.reservedSlots,
        config.priorityAfterTicks,
        config.pushOptEnabled,
        config.disableVec0Push,
        config.maxPushTimesPerTick,
        config.debug
    );

    return true;
}

bool PluginImpl::disable() {
    unregisterHooks();

    auto s = getStats();
    logger().info(
        "Disabled. processed={}, cooldownSkipped={}, throttleSkipped={}, prioritized={}",
        s.totalProcessed,
        s.totalCooldownSkipped,
        s.totalThrottleSkipped,
        s.totalPrioritized
    );

    return true;
}

} // namespace mob_ai_optimizer

LL_REGISTER_MOD(
    mob_ai_optimizer::PluginImpl,
    mob_ai_optimizer::PluginImpl::getInstance()
);
