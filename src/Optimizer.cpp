#include "Optimizer.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/mod/RegisterHelper.h>
#include <ll/api/io/Logger.h>
#include <ll/api/io/LoggerRegistry.h>
#include <ll/api/coro/CoroTask.h>
#include <ll/api/thread/ServerThreadExecutor.h>
#include <mc/world/actor/Mob.h>
#include <mc/world/actor/Actor.h>
#include <mc/world/level/Level.h>
#include <mc/world/level/Tick.h>
#include <mc/legacy/ActorUniqueID.h>
#include <filesystem>
#include <chrono>
#include <algorithm>

namespace mob_ai_optimizer {

// ── 全局变量 ──────────────────────────────────────────────
static Config config;
static std::shared_ptr<ll::io::Logger> log;
static bool debugTaskRunning = false;

std::unordered_map<ActorUniqueID, std::uint64_t> lastAiTick;
int           processedThisTick = 0;
std::uint64_t lastTickId        = 0;
int           cleanupCounter    = 0;

// 动态参数
static int dynMaxPerTick    = 40;
static int dynCooldownTicks = 4;

// 调试统计
static size_t totalProcessed        = 0;
static size_t totalCooldownSkipped  = 0;
static size_t totalThrottleSkipped  = 0;
static size_t totalDespawnCleaned   = 0;
static size_t totalExpiredCleaned   = 0;

// ── Logger ────────────────────────────────────────────────
static ll::io::Logger& getLogger() {
    if (!log) {
        log = ll::io::LoggerRegistry::getInstance().getOrCreate("MobAIOptimizer");
    }
    return *log;
}

// ── Config ────────────────────────────────────────────────
Config& getConfig() { return config; }

bool loadConfig() {
    auto path   = Optimizer::getInstance().getSelf().getConfigDir() / "config.json";
    bool loaded = ll::config::loadConfig(config, path);
    if (config.cleanupIntervalTicks < 1)  config.cleanupIntervalTicks = 100;
    if (config.maxExpiredAge        < 1)  config.maxExpiredAge        = 600;
    if (config.initialMapReserve   == 0)  config.initialMapReserve    = 1000;
    if (config.maxPerTickStep       < 1)  config.maxPerTickStep       = 1;
    if (config.cooldownTicksStep    < 1)  config.cooldownTicksStep    = 1;
    if (config.targetTickMs         < 1)  config.targetTickMs         = 50;
    return loaded;
}

bool saveConfig() {
    auto path = Optimizer::getInstance().getSelf().getConfigDir() / "config.json";
    return ll::config::saveConfig(config, path);
}

// ── Debug ─────────────────────────────────────────────────
static void resetStats() {
    totalProcessed = totalCooldownSkipped = totalThrottleSkipped = 0;
    totalDespawnCleaned = totalExpiredCleaned = 0;
}

static void startDebugTask() {
    if (debugTaskRunning) return;
    debugTaskRunning = true;

    ll::coro::keepThis([]() -> ll::coro::CoroTask<> {
        while (debugTaskRunning) {
            co_await std::chrono::seconds(5);
            ll::thread::ServerThreadExecutor::getDefault().execute([] {
                if (!config.debug) return;
                size_t total = totalProcessed + totalCooldownSkipped + totalThrottleSkipped;
                double skipRate = total > 0
                    ? (100.0 * (totalCooldownSkipped + totalThrottleSkipped) / total)
                    : 0.0;
                getLogger().info(
                    "AI stats (5s): dynMaxPerTick={}, dynCooldown={} | "
                    "processed={}, cooldownSkip={}, throttleSkip={}, "
                    "skipRate={:.1f}%, despawnClean={}, expiredClean={}, tracked={}",
                    dynMaxPerTick, dynCooldownTicks,
                    totalProcessed, totalCooldownSkipped, totalThrottleSkipped,
                    skipRate, totalDespawnCleaned, totalExpiredCleaned,
                    lastAiTick.size()
                );
                resetStats();
            });
        }
        debugTaskRunning = false;
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

static void stopDebugTask() { debugTaskRunning = false; }

// ── 插件生命周期 ──────────────────────────────────────────
Optimizer& Optimizer::getInstance() {
    static Optimizer instance;
    return instance;
}

bool Optimizer::load() {
    std::filesystem::create_directories(getSelf().getConfigDir());
    if (!loadConfig()) {
        getLogger().warn("Failed to load config, using defaults and saving");
        saveConfig();
    }
    lastAiTick.reserve(config.initialMapReserve);
    getLogger().info(
        "Loaded. enabled={}, debug={}, targetTickMs={}, maxPerTickStep={}, cooldownTicksStep={}",
        config.enabled, config.debug,
        config.targetTickMs, config.maxPerTickStep, config.cooldownTicksStep
    );
    return true;
}

bool Optimizer::enable() {
    dynMaxPerTick    = config.maxPerTickStep    * 10;
    dynCooldownTicks = config.cooldownTicksStep * 4;

    if (config.debug) startDebugTask();
    getLogger().info(
        "Enabled. initMaxPerTick={}, initCooldown={}",
        dynMaxPerTick, dynCooldownTicks
    );
    return true;
}

bool Optimizer::disable() {
    stopDebugTask();
    lastAiTick.clear();
    processedThisTick = 0;
    lastTickId        = 0;
    cleanupCounter    = 0;
    resetStats();
    getLogger().info("Disabled");
    return true;
}

} // namespace mob_ai_optimizer

// ── AI 优化 Hook ──────────────────────────────────────────
LL_AUTO_TYPE_INSTANCE_HOOK(
    MobAiStepHook,
    ll::memory::HookPriority::Normal,
    Mob,
    &Mob::$aiStep,
    void
) {
    using namespace mob_ai_optimizer;

    if (!config.enabled) {
        origin();
        return;
    }

    std::uint64_t currentTick = this->getLevel().getCurrentServerTick().tickID;

    if (currentTick != lastTickId) {
        lastTickId        = currentTick;
        processedThisTick = 0;

        if (++cleanupCounter >= config.cleanupIntervalTicks) {
            cleanupCounter = 0;
            for (auto it = lastAiTick.begin(); it != lastAiTick.end();) {
                if (currentTick - it->second >
                    static_cast<std::uint64_t>(config.maxExpiredAge))
                {
                    it = lastAiTick.erase(it);
                    ++totalExpiredCleaned;
                } else {
                    ++it;
                }
            }
        }
    }

    if (processedThisTick >= dynMaxPerTick) {
        ++totalThrottleSkipped;
        return;
    }

    auto [it, inserted] = lastAiTick.emplace(this->getOrCreateUniqueID(), 0);
    if (!inserted &&
        currentTick - it->second <
            static_cast<std::uint64_t>(dynCooldownTicks))
    {
        ++totalCooldownSkipped;
        return;
    }

    ++processedThisTick;
    origin();
    it->second = currentTick;
    ++totalProcessed;
}

// ── Level::tick Hook：测耗时动态调整 ─────────────────────
LL_AUTO_TYPE_INSTANCE_HOOK(
    LevelTickHook,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::$tick,
    void
) {
    using namespace mob_ai_optimizer;

    auto tickStart = std::chrono::steady_clock::now();
    origin();

    if (!config.enabled) return;

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tickStart
    ).count();

    if (elapsed > config.targetTickMs) {
        dynMaxPerTick    = std::max(16, dynMaxPerTick    - config.maxPerTickStep);
        dynCooldownTicks = std::max(1,  dynCooldownTicks + config.cooldownTicksStep);
    } else {
        dynMaxPerTick    += config.maxPerTickStep;
        dynCooldownTicks  = std::max(1, dynCooldownTicks - config.cooldownTicksStep);
    }
}

// ── 自动清理 Hook ─────────────────────────────────────────
LL_AUTO_TYPE_INSTANCE_HOOK(
    ActorDespawnHook,
    ll::memory::HookPriority::Normal,
    Actor,
    &Actor::$despawn,
    void
) {
    using namespace mob_ai_optimizer;
    if (config.enabled) {
        lastAiTick.erase(this->getOrCreateUniqueID());
        ++totalDespawnCleaned;
    }
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
    if (config.enabled) {
        lastAiTick.erase(this->getOrCreateUniqueID());
        ++totalDespawnCleaned;
    }
    origin();
}

// ── 注册插件 ──────────────────────────────────────────────
LL_REGISTER_MOD(mob_ai_optimizer::Optimizer, mob_ai_optimizer::Optimizer::getInstance());
