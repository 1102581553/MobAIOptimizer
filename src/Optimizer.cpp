#include "Optimizer.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/mod/RegisterHelper.h>
#include <ll/api/service/Bedrock.h>
#include <ll/api/io/LoggerRegistry.h>
#include <ll/api/chrono/GameChrono.h>
#include <ll/api/coro/CoroTask.h>
#include <ll/api/thread/ServerThreadExecutor.h>
#include <ll/api/config/Config.h>
#include <mc/world/actor/Mob.h>
#include <mc/world/actor/Actor.h>
#include <mc/world/level/Level.h>
#include <mc/world/level/Tick.h>
#include <mc/legacy/ActorUniqueID.h>
#include <filesystem>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace mob_ai_optimizer {

// ====================== 配置结构 ======================
struct Config {
    bool enabled = true;
    bool debug = false;
    int cooldownTicks = 20;        // AI 冷却时间（tick）
    int maxPerTick = 50;            // 每 tick 最多处理的生物数
    int cleanupInterval = 1000;     // 过期清理间隔（tick）
    int maxExpiredAge = 10000;      // 过期阈值（tick）
};

static Config config;
static std::shared_ptr<ll::io::Logger> log;
static bool hookInstalled = false;
static std::atomic<bool> debugTaskRunning = false;

// 核心数据结构（与原代码一致）
static std::unordered_map<ActorUniqueID, int> lastAiTick;
static std::mutex mapMutex;  // 保护 lastAiTick 的线程安全

// 每 tick 计数器
static int processedThisTick = 0;
static int currentTickId = -1;
static int cleanupCounter = 0;

// 统计信息
static std::atomic<size_t> totalProcessed = 0;       // 总执行次数
static std::atomic<size_t> totalSkippedCooldown = 0; // 因冷却跳过
static std::atomic<size_t> totalSkippedPerTick = 0;  // 因上限跳过
static std::atomic<size_t> totalCacheHit = 0;        // 缓存命中（找到且未过期）
static std::atomic<size_t> totalCacheMiss = 0;       // 缓存未命中（新生物）

// ====================== 辅助函数 ======================
Config& getConfig() { return config; }

ll::io::Logger& logger() {
    if (!log) {
        log = ll::io::LoggerRegistry::getInstance().getOrCreate("MobAIOptimizer");
    }
    return *log;
}

uint64_t getCurrentTickID() {
    auto level = ll::service::getLevel();
    if (!level) return 0;
    return level->getCurrentServerTick().tickID;
}

bool loadConfig() {
    auto path = PluginImpl::getInstance().getSelf().getConfigDir() / "config.json";
    bool loaded = ll::config::loadConfig(config, path);
    // 确保配置值有效
    if (config.cooldownTicks < 1) config.cooldownTicks = 1;
    if (config.maxPerTick < 1) config.maxPerTick = 1;
    return loaded;
}

bool saveConfig() {
    auto path = PluginImpl::getInstance().getSelf().getConfigDir() / "config.json";
    return ll::config::saveConfig(config, path);
}

void clearCache() {
    std::lock_guard<std::mutex> lock(mapMutex);
    lastAiTick.clear();
}

// ====================== 过期清理（与原逻辑一致） ======================
void performCleanup(int currentTick) {
    std::lock_guard<std::mutex> lock(mapMutex);
    for (auto it = lastAiTick.begin(); it != lastAiTick.end(); ) {
        if (currentTick - it->second > config.maxExpiredAge) {
            it = lastAiTick.erase(it);
        } else {
            ++it;
        }
    }
}

// ====================== 调试任务 ======================
void startDebugTask() {
    if (debugTaskRunning.exchange(true)) return;
    ll::coro::keepThis([]() -> ll::coro::CoroTask<> {
        while (debugTaskRunning) {
            co_await std::chrono::seconds(1);
            ll::thread::ServerThreadExecutor::getDefault().execute([]{
                if (!getConfig().debug) return;
                size_t hit = totalCacheHit.load();
                size_t miss = totalCacheMiss.load();
                size_t total = hit + miss;
                double hitRate = total > 0 ? (100.0 * hit / total) : 0.0;
                size_t mapSize;
                {
                    std::lock_guard<std::mutex> lock(mapMutex);
                    mapSize = lastAiTick.size();
                }
                logger().info("Cache stats: size={}, hit={}, miss={}, processed={}, skipCooldown={}, skipPerTick={}, hitRate={:.1f}%",
                              mapSize,
                              hit, miss,
                              totalProcessed.load(), totalSkippedCooldown.load(), totalSkippedPerTick.load(),
                              hitRate);
            });
        }
        debugTaskRunning = false;
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

void stopDebugTask() {
    debugTaskRunning = false;
}

// ====================== 钩子定义 ======================
// Mob::aiStep 钩子
LL_TYPE_INSTANCE_HOOK(
    MobAiStepHook,
    ll::memory::HookPriority::Normal,
    Mob,
    &Mob::$aiStep,
    void
) {
    if (!getConfig().enabled) {
        origin();
        return;
    }

    auto* self = this;
    auto& level = self->getLevel();
    auto currentTick = level.getCurrentServerTick().tickID;
    int tickInt = static_cast<int>(currentTick);

    // 每 tick 重置计数器
    if (tickInt != currentTickId) {
        currentTickId = tickInt;
        processedThisTick = 0;
        cleanupCounter++;
        if (cleanupCounter >= config.cleanupInterval) {
            performCleanup(tickInt);
            cleanupCounter = 0;
        }
    }

    ActorUniqueID id = self->getOrCreateUniqueID();

    // 检查冷却
    bool shouldExecute = true;
    {
        std::lock_guard<std::mutex> lock(mapMutex);
        auto it = lastAiTick.find(id);
        if (it != lastAiTick.end()) {
            if (tickInt - it->second < config.cooldownTicks) {
                shouldExecute = false;
                ++totalSkippedCooldown;
                if (config.debug) {
                    logger().debug("Skip AI (cooldown) for entity {} at tick {}", id.id, tickInt);
                }
            }
            ++totalCacheHit;
        } else {
            ++totalCacheMiss;
            if (config.debug) {
                logger().debug("Cache miss for entity {}, will execute AI", id.id);
            }
        }
    }

    // 检查每 tick 上限
    if (shouldExecute && processedThisTick >= config.maxPerTick) {
        shouldExecute = false;
        ++totalSkippedPerTick;
        if (config.debug) {
            logger().debug("Skip AI (per-tick limit) for entity {} at tick {}", id.id, tickInt);
        }
    }

    if (!shouldExecute) {
        return;
    }

    // 执行原始 AI
    processedThisTick++;
    totalProcessed++;
    origin();

    // 更新 lastAiTick
    {
        std::lock_guard<std::mutex> lock(mapMutex);
        lastAiTick[id] = tickInt;
    }

    if (config.debug) {
        logger().debug("Executed AI for entity {} at tick {}", id.id, tickInt);
    }
}

// Actor::despawn 钩子
LL_TYPE_INSTANCE_HOOK(
    ActorDespawnHook,
    ll::memory::HookPriority::Normal,
    Actor,
    &Actor::$despawn,
    void
) {
    if (getConfig().enabled) {
        ActorUniqueID id = this->getOrCreateUniqueID();
        std::lock_guard<std::mutex> lock(mapMutex);
        lastAiTick.erase(id);
    }
    origin();
}

// Actor::remove 钩子
LL_TYPE_INSTANCE_HOOK(
    ActorRemoveHook,
    ll::memory::HookPriority::Normal,
    Actor,
    &Actor::$remove,
    void
) {
    if (getConfig().enabled) {
        ActorUniqueID id = this->getOrCreateUniqueID();
        std::lock_guard<std::mutex> lock(mapMutex);
        lastAiTick.erase(id);
    }
    origin();
}

// ====================== 插件主类 ======================
class PluginImpl {

public:
    static PluginImpl& getInstance() {
        static PluginImpl instance;
        return instance;
    }

    ll::mod::Mod& getSelf() const { return *mSelf; }

    bool load(ll::mod::Mod& self) {
        mSelf = &self;
        std::filesystem::create_directories(getSelf().getConfigDir());
        if (!loadConfig()) {
            logger().warn("Failed to load config, using default values and saving");
            saveConfig();
        }
        logger().info("Plugin loaded. enabled: {}, debug: {}, cooldown: {}, maxPerTick: {}, cleanupInterval: {}, maxExpiredAge: {}",
                      config.enabled, config.debug, config.cooldownTicks, config.maxPerTick,
                      config.cleanupInterval, config.maxExpiredAge);
        return true;
    }

    bool enable() {
        if (!hookInstalled) {
            MobAiStepHook::hook();
            ActorDespawnHook::hook();
            ActorRemoveHook::hook();
            hookInstalled = true;
            logger().debug("Hooks installed");
        }
        if (config.debug) {
            startDebugTask();
        }
        logger().info("Plugin enabled");
        return true;
    }

    bool disable() {
        stopDebugTask();
        if (hookInstalled) {
            MobAiStepHook::unhook();
            ActorDespawnHook::unhook();
            ActorRemoveHook::unhook();
            hookInstalled = false;
            clearCache();

            // 重置计数器
            totalProcessed = 0;
            totalSkippedCooldown = 0;
            totalSkippedPerTick = 0;
            totalCacheHit = 0;
            totalCacheMiss = 0;
            processedThisTick = 0;
            currentTickId = -1;
            cleanupCounter = 0;
            logger().debug("Hooks uninstalled, cache cleared, counters reset");
        }
        logger().info("Plugin disabled");
        return true;
    }

private:
    ll::mod::Mod* mSelf = nullptr;
};

} // namespace mob_ai_optimizer

LL_REGISTER_MOD(mob_ai_optimizer::PluginImpl, mob_ai_optimizer::PluginImpl::getInstance());
