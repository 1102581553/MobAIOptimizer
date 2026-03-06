#include "Optimizer.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/mod/RegisterHelper.h>
#include <ll/api/io/Logger.h>
#include <ll/api/io/LoggerRegistry.h>
#include <ll/api/coro/CoroTask.h>
#include <ll/api/thread/ServerThreadExecutor.h>
#include <mc/world/actor/Mob.h>
#include <mc/world/actor/Actor.h>
#include <mc/world/actor/ActorType.h>          // 用于 isType 判断
#include <mc/world/level/Level.h>
#include <mc/world/level/Tick.h>
#include <mc/legacy/ActorUniqueID.h>
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <thread>
#include <future>
#include <vector>

namespace mob_ai_optimizer {

// 全局配置
static Config config;
static std::shared_ptr<ll::io::Logger> log;
static bool debugTaskRunning = false;

// 冷却记录（全局，用于清理钩子）
std::unordered_map<ActorUniqueID, std::uint64_t> lastAiTick;
static std::mutex lastAiTickMutex;

// 调试统计
static size_t totalProcessed        = 0;
static size_t totalCooldownSkipped  = 0;
static size_t totalDespawnCleaned   = 0;

// Logger
static ll::io::Logger& getLogger() {
    if (!log) {
        log = ll::io::LoggerRegistry::getInstance().getOrCreate("MobAIOptimizer");
    }
    return *log;
}

Config& getConfig() { return config; }

bool loadConfig() {
    auto path = Optimizer::getInstance().getSelf().getConfigDir() / "config.json";
    bool loaded = ll::config::loadConfig(config, path);
    // 确保配置有效性
    if (config.aiCooldownTicks < 1) config.aiCooldownTicks = 4;
    if (config.initialMapReserve == 0) config.initialMapReserve = 1000;
    return loaded;
}

bool saveConfig() {
    auto path = Optimizer::getInstance().getSelf().getConfigDir() / "config.json";
    return ll::config::saveConfig(config, path);
}

// 调试统计重置
static void resetStats() {
    totalProcessed = totalCooldownSkipped = 0;
    totalDespawnCleaned = 0;
}

// 调试任务（每5秒输出统计）
static void startDebugTask() {
    if (debugTaskRunning) return;
    debugTaskRunning = true;

    ll::coro::keepThis([]() -> ll::coro::CoroTask<> {
        while (debugTaskRunning) {
            co_await std::chrono::seconds(5);
            ll::thread::ServerThreadExecutor::getDefault().execute([] {
                if (!config.debug) return;
                size_t total = totalProcessed + totalCooldownSkipped;
                double skipRate = total > 0 ? (100.0 * totalCooldownSkipped / total) : 0.0;
                getLogger().info(
                    "AI stats (5s): processed={}, cooldownSkip={}, skipRate={:.1f}%, despawnClean={}, tracked={}",
                    totalProcessed, totalCooldownSkipped, skipRate,
                    totalDespawnCleaned, lastAiTick.size()
                );
                resetStats();
            });
        }
        debugTaskRunning = false;
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

static void stopDebugTask() { debugTaskRunning = false; }

// ==================== 并行处理 ====================

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
        if (!actor) continue;

        // 确保是 Mob
        if (!actor->isType(::ActorType::Mob)) continue;
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

        // 执行 AI
        mob->aiStep();

        // 记录冷却
        tlCoolDownMap[uid] = currentTick;
        result.processed++;
    }

    return result;
}

// 获取所有生物（使用 Level::getRuntimeActorList）
static std::vector<Actor*> collectAllMobs(Level& level) {
    std::vector<Actor*> mobs;
    auto actors = level.getRuntimeActorList();
    for (Actor* actor : actors) {
        if (actor && actor->isType(::ActorType::Mob)) {
            mobs.push_back(actor);
        }
    }
    return mobs;
}

static void parallelProcessMobAI(Level& level, std::uint64_t currentTick, int cooldownTicks) {
    std::vector<Actor*> mobs = collectAllMobs(level);
    if (mobs.empty()) return;

    const size_t numThreads = std::max(1u, std::thread::hardware_concurrency());
    const size_t mobsPerThread = (mobs.size() + numThreads - 1) / numThreads;

    std::vector<std::future<WorkerResult>> futures;
    futures.reserve(numThreads);

    std::vector<ActorUniqueID> processedIds;
    std::mutex processedIdsMutex;

    for (size_t t = 0; t < numThreads; ++t) {
        size_t startIdx = t * mobsPerThread;
        size_t endIdx = std::min(startIdx + mobsPerThread, mobs.size());
        if (startIdx >= mobs.size()) break;

        // 按引用捕获 mobs（确保迭代器类型正确）
        futures.push_back(std::async(std::launch::async, [&mobs, startIdx, endIdx, currentTick, cooldownTicks, &processedIds, &processedIdsMutex]() {
            auto result = workerProcessMobRange(mobs.begin() + startIdx, mobs.begin() + endIdx,
                                                 currentTick, cooldownTicks);
            // 收集 ID
            std::lock_guard lock(processedIdsMutex);
            for (size_t i = startIdx; i < endIdx; ++i) {
                Actor* actor = mobs[i];
                if (actor && actor->isType(::ActorType::Mob)) {
                    processedIds.push_back(actor->getOrCreateUniqueID());
                }
            }
            return result;
        }));
    }

    // 汇总结果
    for (auto& future : futures) {
        auto result = future.get();
        totalProcessed += result.processed;
        totalCooldownSkipped += result.skipped;
    }

    // 更新全局冷却表（供清理钩子使用）
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
    getLogger().info("Loaded. enabled={}, debug={}, aiCooldownTicks={}",
                     config.enabled, config.debug, config.aiCooldownTicks);
    return true;
}

bool Optimizer::enable() {
    if (config.debug) startDebugTask();
    getLogger().info("Enabled.");
    return true;
}

bool Optimizer::disable() {
    stopDebugTask();
    lastAiTick.clear();
    resetStats();
    getLogger().info("Disabled");
    return true;
}

} // namespace mob_ai_optimizer

// ==================== 钩子 ====================

// 禁用原版 AI（插件启用时直接返回）
LL_AUTO_TYPE_INSTANCE_HOOK(
    MobAiStepHook,
    ll::memory::HookPriority::Normal,
    Mob,
    &Mob::$aiStep,
    void
) {
    using namespace mob_ai_optimizer;
    if (config.enabled) {
        return; // 由并行任务处理，禁止原版调用
    }
    origin();
}

// Level::tick 钩子：在 tick 开始时并行处理 AI
LL_AUTO_TYPE_INSTANCE_HOOK(
    LevelTickHook,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::$tick,
    void
) {
    using namespace mob_ai_optimizer;

    if (config.enabled) {
        std::uint64_t currentTick = getCurrentServerTick().tickID;
        parallelProcessMobAI(*this, currentTick, config.aiCooldownTicks);
    }

    origin(); // 执行原版 tick
}

// 清理钩子：生物消失时从全局冷却表中移除
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
