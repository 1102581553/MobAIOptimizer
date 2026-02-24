#include "Optimizer.h"

#include "ll/api/memory/Hook.h"
#include "ll/api/plugin/NativePlugin.h"
#include "ll/api/plugin/RegisterHelper.h"
#include "ll/api/Logger.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/level/BlockSource.h"
#include "mc/world/level/Level.h"

#include <unordered_map>

namespace mob_ai_optimizer {

// ── 全局状态 ──────────────────────────────────────────────────────────────────
Config config{};

static ll::Logger logger("MobAIOptimizer");

static std::uint64_t lastTickId        = 0;
static int           processedThisTick = 0;
static std::unordered_map<std::int64_t, std::uint64_t> lastAiTick;
static Stats gStats{};

Stats getStats()   { return gStats; }
void  resetStats() { gStats = {}; }

// ── Hook ──────────────────────────────────────────────────────────────────────
LL_AUTO_TYPE_INSTANCE_HOOK(
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

    const std::uint64_t currentTick = this->getLevel().getCurrentServerTick().tickID;

    if (currentTick != lastTickId) {
        lastTickId        = currentTick;
        processedThisTick = 0;
    
        // 每秒清理一次，避免每 tick O(N)
        if (currentTick % 20 == 0) {
    
            // 过期时间必须 >= cooldownTicks
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
void registerHooks() { ActorTickHook::hook(); }
void unregisterHooks() { ActorTickHook::unhook(); }

// ── 插件入口 ──────────────────────────────────────────────────────────────────
namespace {

bool load(ll::plugin::NativePlugin& self) {
    const auto configPath = self.getConfigDir() / "config.json";
    if (!ll::config::loadConfig(config, configPath)) {
        logger.warn("Failed to load config, using defaults.");
        ll::config::saveConfig(config, configPath);
    }
    return true;
}

bool enable(ll::plugin::NativePlugin& /*self*/) {
    registerHooks();
    logger.info(
        "Enabled. maxPerTick={}, cooldownTicks={}",
        config.maxPerTick,
        config.cooldownTicks
    );
    return true;
}

bool disable(ll::plugin::NativePlugin& /*self*/) {
    unregisterHooks();
    auto s = getStats();
    logger.info(
        "Disabled. processed={}, cooldownSkipped={}, throttleSkipped={}",
        s.totalProcessed,
        s.totalCooldownSkipped,
        s.totalThrottleSkipped
    );
    return true;
}

} // namespace

} // namespace mob_ai_optimizer

LL_REGISTER_PLUGIN(mob_ai_optimizer::load,
                   mob_ai_optimizer::enable,
                   mob_ai_optimizer::disable);
