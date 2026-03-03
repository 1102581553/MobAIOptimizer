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
#include <thread>
#include <future>
#include <vector>
#include <numeric>

namespace mob_ai_optimizer {

// 全局
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

// 全局锁
static std::mutex lastAiTickMutex;

// Logger
static ll::io::Logger& getLogger() {
    if (!log) {
        log = ll::io::LoggerRegistry::getInstance().getOrCreate("MobAIOptimizer");
    }
    return *log;
}

// Config
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

// Debug
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

// ==================== 并行化实现 ====================
// 并行永远启用

struct WorkerResult {
    size_t processed;
    size_t skipped;
};

// 线程局部冷却表
static thread_local std::unordered_map<ActorUniqueID, std::uint64_t> tlCoolDownMap;

static WorkerResult workerProcessMobRange(
    std::vector<Actor*>::iterator start,
    std::vector<Actor*>::iterator end,
    std::uint64_t currentTick,
    int cooldownTicks
) {
    WorkerResult result{0, 0};

    for (auto it = start; it != end; ++it) {
        Actor* actor = *it;
        if (!actor || actor->isDead()) continue;

        Mob* mob = static_cast<Mob*>(actor);

        ActorUniqueID uid = mob->getOrCreateUniqueID();

        // 检查冷却（线程局部）
        auto cdIt = tlCoolDownMap.find(uid);
        bool inCooldown = (cdIt != tlCoolDownMap.end() &&
                          currentTick - cdIt->second < static_cast<std::uint64_t>(cooldownTicks));

        if (inCooldown) {
            result.skipped++;
            continue;
        }

        // 执行AI计算
        mob->aiStep();

        // 记录冷却
        tlCoolDownMap[uid] = currentTick;
        result.processed++;
    }

    return result;
}

static std::vector<Actor*> collectAllMobs(Level& level) {
    std::vector<Actor*> mobs;
    auto& registry = level.getActorRegistry();
    for (auto& [uid, actor] : registry) {
        if (!actor) continue;
        if (actor->isMob()) {
            mobs.push_back(actor);
        }
    }
    return mobs;
}

static void parallelProcessMobAI(Level& level, std::uint64_t currentTick, int cooldownTicks) {
    std::vector<Actor*> mobs = collectAllMobs(level);

    if (mobs.empty()) return;

    // 使用CPU核心数作为线程数
    const size_t numThreads = std::max(1u, std::thread::hardware_concurrency());
    const size_t mobsPerThread = (mobs.size() + numThreads - 1) / numThreads;

    std::vector<std::future<WorkerResult>> futures;
    futures.reserve(numThreads);

    // 收集所有处理的 Mob ID，用于更新全局冷却表
    std::vector<ActorUniqueID> processedIds;
    std::mutex processedIdsMutex;

    // 分发任务到线程池
    for (size_t t = 0; t < numThreads; ++t) {
        size_t startIdx = t * mobsPerThread;
        size_t endIdx = std::min(startIdx + mobsPerThread, mobs.size());

        if (startIdx >= mobs.size()) break;

        futures.push_back(std::async(std::launch::async, [=, &processedIds, &processedIdsMutex]() {
            auto result = workerProcessMobRange(mobs.begin() + startIdx, mobs.begin() + endIdx,
                                         currentTick, cooldownTicks);

            // 收集处理的 ID
            std::lock_guard lock(processedIdsMutex);
            for (size_t i = startIdx; i < endIdx; ++i) {
                Actor* actor = mobs[i];
                if (actor && actor->isMob()) {
                    processedIds.push_back(actor->getOrCreateUniqueID());
                }
            }

            return result;
        }));
    }

    // 收集结果
    for (auto& future : futures) {
        auto result = future.get();
        totalProcessed += result.processed;
        totalThrottleSkipped += result.skipped;
    }

    // 更新全局冷却表，防止 origin() 中的 MobAIHook 重复执行
    std::lock_guard<std::mutex> lock(lastAiTickMutex);
    for (ActorUniqueID uid : processedIds) {
        lastAiTick[uid] = currentTick;
    }
}

// 插件生命周期
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

// AI 优化 Hook（并行模式下永远不执行，由 Level Tick 统一并行处理）
LL_AUTO_TYPE_INSTANCE_HOOK(
    MobAiStepHook,
    ll::memory::HookPriority::Normal,
    Mob,
    &Mob::$aiStep,
    void
) {
    // 并行模式永远启用，此 Hook 永远不执行
    origin();
}

// Level::tick Hook
LL_AUTO_TYPE_INSTANCE_HOOK(
    LevelTickHook,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::$tick,
    void
) {
    using namespace mob_ai_optimizer;

    auto tickStart = std::chrono::steady_clock::now();

    // 始终使用并行处理 Mob AI
    if (config.enabled) {
        std::uint64_t currentTick = getCurrentServerTick().tickID;
        parallelProcessMobAI(*this, currentTick, dynCooldownTicks);
    }

    origin();

    if (!config.enabled) return;

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tickStart
    ).count();

    // 动态调整参数
    if (elapsed > config.targetTickMs) {
        dynMaxPerTick    = std::max(16, dynMaxPerTick    - config.maxPerTickStep);
        dynCooldownTicks = std::max(1,  dynCooldownTicks + config.cooldownTicksStep);
    } else if (elapsed < config.targetTickMs * 0.5) {
        dynMaxPerTick    += config.maxPerTickStep;
        dynCooldownTicks  = std::max(1, dynCooldownTicks - config.cooldownTicksStep);
    }
}

// 清理 Hook
LL_AUTO_TYPE_INSTANCE_HOOK(
    ActorDespawnHook,
    ll::memory::HookPriority::Normal,
    Actor,
    &Actor::$despawn,
    void
) {
    using namespace mob_ai_optimizer;
    if (config.enabled) {
        std::lock_guard<std::mutex> lock(lastAiTickMutex);
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
        std::lock_guard<std::mutex> lock(lastAiTickMutex);
        lastAiTick.erase(this->getOrCreateUniqueID());
        ++totalDespawnCleaned;
    }
    origin();
}

LL_REGISTER_MOD(mob_ai_optimizer::Optimizer, mob_ai_optimizer::Optimizer::getInstance());
