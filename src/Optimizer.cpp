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
#include <list>
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
    size_t maxCacheSize = 10000;    // 缓存最大条目数
};

// ====================== 全局变量 ======================
static Config config;
static std::shared_ptr<ll::io::Logger> log;
static bool hookInstalled = false;
static std::atomic<bool> debugTaskRunning = false;

// LRU 缓存结构
static std::list<ActorUniqueID> lruList;                                   // 链表头部最新，尾部最旧
static std::unordered_map<ActorUniqueID, std::pair<int, std::list<ActorUniqueID>::iterator>> cacheMap; // 键 -> (lastAiTick, 链表迭代器)
static std::mutex cacheMutex;

// 每 tick 计数器
static int processedThisTick = 0;
static int currentTickId = -1;
static int cleanupCounter = 0;

// 统计信息
static std::atomic<size_t> totalProcessed = 0;       // 总执行次数
static std::atomic<size_t> totalSkippedCooldown = 0; // 因冷却跳过
static std::atomic<size_t> totalSkippedPerTick = 0;  // 因上限跳过
static std::atomic<size_t> totalCacheHit = 0;        // 缓存命中（找到且未过期）
static std::atomic<size_t> totalCacheMiss = 0;       // 缓存未命中（新生物或已淘汰）

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
    if (config.maxCacheSize == 0) config.maxCacheSize = 10000;
    return loaded;
}

bool saveConfig() {
    auto path = PluginImpl::getInstance().getSelf().getConfigDir() / "config.json";
    return ll::config::saveConfig(config, path);
}

void clearCache() {
    std::lock_guard<std::mutex> lock(cacheMutex);
    lruList.clear();
    cacheMap.clear();
}

// ====================== 调试任务 ======================
void startDebugTask() {
    if (debugTaskRunning.exchange(true)) return;
    ll::coro::keepThis([]() -> ll::coro::CoroTask<> {
        while (debugTaskRunning) {
            co_await std::chrono::seconds(1);
            ll::thread::ServerThreadExecutor::getDefault().execute([]{
                if (!getConfig().debug) return;
                std::lock_guard<std::mutex> lock(cacheMutex);
                size_t total = totalCacheHit + totalCacheMiss;
                double hitRate = total > 0 ? (100.0 * totalCacheHit / total) : 0.0;
                logger().info("Cache stats: size={}, hit={}, miss={}, processed={}, skipCooldown={}, skipPerTick={}, hitRate={:.1f}%",
                              cacheMap.size(),
                              totalCacheHit.load(), totalCacheMiss.load(),
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

// ====================== 过期清理（后备） ======================
void performCleanup(int currentTick) {
    std::lock_guard<std::mutex> lock(cacheMutex);
    for (auto it = cacheMap.begin(); it != cacheMap.end(); ) {
        if (currentTick - it->second.first > config.maxExpiredAge) {
            lruList.erase(it->second.second);
            it = cacheMap.erase(it);
        } else {
            ++it;
        }
    }
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

    // 缓存查找与更新（LRU）
    bool shouldExecute = true;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);

        auto it = cacheMap.find(id);
        if (it != cacheMap.end()) {
            // 命中缓存：将节点移到链表头部
            lruList.splice(lruList.begin(), lruList, it->second.second);
            int lastTick = it->second.first;

            // 检查冷却
            if (tickInt - lastTick < config.cooldownTicks) {
                shouldExecute = false;
                ++totalSkippedCooldown;
                if (config.debug) {
                    logger().debug("Skip AI (cooldown) for entity {} at tick {}", id.id, tickInt);
                }
            }
            ++totalCacheHit;
        } else {
            // 未命中：新生物或已被淘汰
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

    // 更新缓存中的 lastAiTick
    {
        std::lock_guard<std::mutex> lock(cacheMutex);

        auto it = cacheMap.find(id);
        if (it != cacheMap.end()) {
            // 更新已有条目
            it->second.first = tickInt;
            // 已通过 splice 移到头部
        } else {
            // 插入新条目，并检查缓存大小
            // 如果超过最大缓存，淘汰最久未使用的（链表尾部）
            if (cacheMap.size() >= config.maxCacheSize) {
                ActorUniqueID oldestKey = lruList.back();
                cacheMap.erase(oldestKey);
                lruList.pop_back();
            }
            // 插入头部
            lruList.push_front(id);
            cacheMap[id] = {tickInt, lruList.begin()};
        }
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
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = cacheMap.find(id);
        if (it != cacheMap.end()) {
            lruList.erase(it->second.second);
            cacheMap.erase(it);
        }
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
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = cacheMap.find(id);
        if (it != cacheMap.end()) {
            lruList.erase(it->second.second);
            cacheMap.erase(it);
        }
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
        logger().info("Plugin loaded. enabled: {}, debug: {}, cooldown: {}, maxPerTick: {}, maxCacheSize: {}",
                      config.enabled, config.debug, config.cooldownTicks, config.maxPerTick, config.maxCacheSize);
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
