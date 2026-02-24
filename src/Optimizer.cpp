#include "Optimizer.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/io/LoggerRegistry.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"

#include <filesystem>
#include <unordered_map>

namespace mob_ai_optimizer {

// ── 每个实体的状态 ────────────────────────────────────────
struct ActorState {
    std::uint64_t lastAiTick   = 0; // 上次实际执行 AI 的 tick
    std::uint64_t pendingSince = 0; // 开始被 throttle 的 tick，0 = 未在等待
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

// ── Hook ──────────────────────────────────────────────────
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
    // 普通实体：只能用前 (maxPerTick - reservedSlots) 个配额
    // 优先实体：可以用到全部 maxPerTick 配额
    const int normalLimit   = config.maxPerTick - config.reservedSlots;
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

// ── 注册 / 注销 ───────────────────────────────────────────
void registerHooks()   { ActorTickHook::hook(); }
void unregisterHooks() { ActorTickHook::unhook(); }

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
    // 防止 reservedSlots 配置错误导致 normalLimit 为负
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
        "Enabled. maxPerTick={}, cooldownTicks={}, reservedSlots={}, priorityAfterTicks={}, debug={}",
        config.maxPerTick,
        config.cooldownTicks,
        config.reservedSlots,
        config.priorityAfterTicks,
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
