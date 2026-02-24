// Optimizer.cpp
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

// ── 全局状态 ─────────────────────────────────────────────
static Config                          config;
static std::shared_ptr<ll::io::Logger> log;
static Stats                           gStats{};

static std::uint64_t lastTickId        = 0;
static int           processedThisTick = 0;

static std::uint64_t lastDebugTick     = 0;
static std::uint64_t lastCleanupTick   = 0;

static std::unordered_map<std::int64_t, std::uint64_t> lastAiTick;

// ────────────────────────────────────────────────────────

Config& getConfig() { return config; }

ll::io::Logger& logger() {
    if (!log) {
        log = ll::io::LoggerRegistry::getInstance().getOrCreate("MobAIOptimizer");
    }
    return *log;
}

Stats getStats()   { return gStats; }
void  resetStats() { gStats = {}; }

// ── Hook ────────────────────────────────────────────────
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

    // ── Tick 切换 ───────────────────────────────────────
    if (currentTick != lastTickId) {
        lastTickId        = currentTick;
        processedThisTick = 0;

        // ── 定时清理 UID 表 ─────────────────────────────
        const std::uint64_t cleanupInterval =
            static_cast<std::uint64_t>(config.cleanupIntervalSeconds) * 20;

        if (currentTick - lastCleanupTick >= cleanupInterval) {
            lastCleanupTick = currentTick;

            const std::uint64_t expiry =
                static_cast<std::uint64_t>(config.cooldownTicks) *
                config.expiryMultiplier;

            for (auto it = lastAiTick.begin(); it != lastAiTick.end();) {
                if (currentTick - it->second > expiry)
                    it = lastAiTick.erase(it);
                else
                    ++it;
            }
        }

        // ── Debug 日志 ─────────────────────────────────
        if (config.debug) {
            const std::uint64_t debugInterval =
                static_cast<std::uint64_t>(config.debugLogIntervalSeconds) * 20;

            if (currentTick - lastDebugTick >= debugInterval) {
                lastDebugTick = currentTick;

                logger().info(
                    "[Debug] processed={}, cooldownSkipped={}, throttleSkipped={}, cacheSize={}",
                    gStats.totalProcessed,
                    gStats.totalCooldownSkipped,
                    gStats.totalThrottleSkipped,
                    lastAiTick.size()
                );
            }
        }
    }

    // ── 全局限流 ───────────────────────────────────────
    if (processedThisTick >= config.maxPerTick) {
        ++gStats.totalThrottleSkipped;
        return true;
    }

    const std::int64_t uid = this->getOrCreateUniqueID().rawID;
    auto [it, inserted]    = lastAiTick.emplace(uid, 0);

    // ── 冷却判断 ───────────────────────────────────────
    if (!inserted &&
        currentTick - it->second <
            static_cast<std::uint64_t>(config.cooldownTicks))
    {
        ++gStats.totalCooldownSkipped;
        return true;
    }

    ++processedThisTick;

    bool result = origin(region);
    it->second  = currentTick;

    ++gStats.totalProcessed;
    return result;
}

// ── 注册 / 注销 ────────────────────────────────────────
void registerHooks()   { ActorTickHook::hook(); }
void unregisterHooks() { ActorTickHook::unhook(); }

// ── PluginImpl ─────────────────────────────────────────
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
    registerHooks();

    logger().info(
        "Enabled. maxPerTick={}, cooldownTicks={}, debug={}",
        config.maxPerTick,
        config.cooldownTicks,
        config.debug
    );

    return true;
}

bool PluginImpl::disable() {
    unregisterHooks();

    auto s = getStats();
    logger().info(
        "Disabled. processed={}, cooldownSkipped={}, throttleSkipped={}",
        s.totalProcessed,
        s.totalCooldownSkipped,
        s.totalThrottleSkipped
    );

    return true;
}

} // namespace mob_ai_optimizer

LL_REGISTER_MOD(
    mob_ai_optimizer::PluginImpl,
    mob_ai_optimizer::PluginImpl::getInstance()
);
