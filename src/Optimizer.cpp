// Optimizer.cpp
#include "Optimizer.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/io/Logger.h"
#include "ll/api/io/LoggerRegistry.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"


#include <filesystem>
#include <unordered_map>

namespace mob_ai_optimizer {

// ── 全局状态 ──────────────────────────────────────────────────────────────────
static Config                          config;
static std::shared_ptr<ll::io::Logger> log;
static Stats                           gStats{};

static std::uint64_t lastTickId        = 0;
static int           processedThisTick = 0;
static std::unordered_map<std::int64_t, std::uint64_t> lastAiTick;

Config& getConfig() { return config; }

ll::io::Logger& logger() {
    if (!log) {
        log = ll::io::LoggerRegistry::getInstance().getOrCreate("MobAIOptimizer");
    }
    return *log;
}

Stats getStats()   { return gStats; }
void  resetStats() { gStats = {}; }

// ── Hook ──────────────────────────────────────────────────────────────────────
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

    const std::uint64_t currentTick = this->getLevel().getCurrentTick().tickID;

    if (currentTick != lastTickId) {
        lastTickId        = currentTick;
        processedThisTick = 0;

        if (currentTick % 20 == 0) {
            const std::uint64_t kExpiry =
                std::max<std::uint64_t>(
                    static_cast<std::uint64_t>(config.cooldownTicks) * 2,
                    60ULL
                );

            for (auto it = lastAiTick.begin(); it != lastAiTick.end(); ) {
                if (currentTick - it->second > kExpiry)
                    it = lastAiTick.erase(it);
                else
                    ++it;
            }
        }
    }

    if (processedThisTick >= config.maxPerTick) {
        ++gStats.totalThrottleSkipped;
        return true;
    }

    const std::int64_t uid = this->getOrCreateUniqueID().rawID;
    auto [it, inserted]    = lastAiTick.emplace(uid, 0);

    if (!inserted &&
        currentTick - it->second < static_cast<std::uint64_t>(config.cooldownTicks))
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

// ── 注册 / 注销 ───────────────────────────────────────────────────────────────
void registerHooks()   { ActorTickHook::hook(); }
void unregisterHooks() { ActorTickHook::unhook(); }

// ── PluginImpl ────────────────────────────────────────────────────────────────
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
        "Enabled. maxPerTick={}, cooldownTicks={}",
        config.maxPerTick,
        config.cooldownTicks
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

LL_REGISTER_MOD(mob_ai_optimizer::PluginImpl, mob_ai_optimizer::PluginImpl::getInstance());
