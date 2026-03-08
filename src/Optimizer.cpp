#include "Optimizer.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/mod/RegisterHelper.h>
#include <ll/api/io/Logger.h>
#include <ll/api/io/LoggerRegistry.h>
#include <ll/api/coro/CoroTask.h>
#include <ll/api/thread/ServerThreadExecutor.h>
#include <mc/world/actor/Mob.h>
#include <mc/world/actor/Actor.h>
#include <mc/world/actor/ActorCategory.h>
#include <mc/world/level/Level.h>
#include <mc/world/level/Tick.h>
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <thread>
#include <future>
#include <vector>
#include <windows.h>
#include <unordered_set>

namespace mob_ai_optimizer {

// 配置结构（需在 Optimizer.h 中定义）
static Config config;
static std::shared_ptr<ll::io::Logger> log;
static bool debugTaskRunning = false;

// 调试统计
static size_t totalProcessed = 0;
static size_t totalTicks = 0;
static size_t lastMobCount = 0;

// 已崩溃生物 ID 黑名单（全局，线程安全）
static std::unordered_set<ActorUniqueID> crashedIds;
static std::mutex crashedIdsMutex;

static ll::io::Logger& getLogger() {
    if (!log) {
        log = ll::io::LoggerRegistry::getInstance().getOrCreate("MobAIOptimizer");
    }
    return *log;
}

Config& getConfig() { return config; }

bool loadConfig() {
    auto path = Optimizer::getInstance().getSelf().getConfigDir() / "config.json";
    return ll::config::loadConfig(config, path);
}

bool saveConfig() {
    auto path = Optimizer::getInstance().getSelf().getConfigDir() / "config.json";
    return ll::config::saveConfig(config, path);
}

static void resetStats() {
    totalProcessed = 0;
    totalTicks = 0;
}

static void startDebugTask() {
    if (debugTaskRunning) return;
    debugTaskRunning = true;

    ll::coro::keepThis([]() -> ll::coro::CoroTask<> {
        while (debugTaskRunning) {
            co_await std::chrono::seconds(5);
            ll::thread::ServerThreadExecutor::getDefault().execute([] {
                if (!config.debug) return;
                size_t avgPerTick = totalTicks > 0 ? totalProcessed / totalTicks : 0;
                getLogger().info(
                    "AI stats (5s): total processed={}, avg per tick={}, last mob count={}",
                    totalProcessed, avgPerTick, lastMobCount
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
};

// 纯 C 风格的 SEH 安全调用
struct SafeAiStepResult {
    bool success;
    DWORD exceptionCode;
};

static SafeAiStepResult safeAiStep(Mob* mob) {
    __try {
        mob->aiStep();
        return {true, 0};
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DWORD code = GetExceptionCode();
        return {false, code};
    }
}

static WorkerResult workerProcessMobRange(
    std::vector<Actor*>::iterator start,
    std::vector<Actor*>::iterator end
) {
    WorkerResult result{0};

    for (auto it = start; it != end; ++it) {
        Actor* actor = *it;
        if (!actor) continue;

        if (!actor->hasCategory(::ActorCategory::Mob)) continue;
        Mob* mob = static_cast<Mob*>(actor);
        ActorUniqueID uid = mob->getOrCreateUniqueID();

        // 检查黑名单
        {
            std::lock_guard<std::mutex> lock(crashedIdsMutex);
            if (crashedIds.find(uid) != crashedIds.end()) {
                continue;
            }
        }

        auto safeResult = safeAiStep(mob);
        if (safeResult.success) {
            result.processed++;
        } else {
            // 加入黑名单
            {
                std::lock_guard<std::mutex> lock(crashedIdsMutex);
                crashedIds.insert(uid);
            }
            getLogger().error("SEH exception in aiStep for mob {} (UID {}): code 0x{:X} - added to blacklist",
                              (uint64_t)mob, uid.rawID, safeResult.exceptionCode);
        }
    }

    return result;
}

// 修改点1：收集生物时排除玩家
static std::vector<Actor*> collectAllMobs(Level& level) {
    std::vector<Actor*> mobs;
    auto actors = level.getRuntimeActorList();

    for (Actor* actor : actors) {
        if (!actor) continue;
        // 只收集 Mob 类别且不是玩家的实体
        if (actor->hasCategory(::ActorCategory::Mob) && !actor->isPlayer()) {
            mobs.push_back(actor);
        }
    }

    lastMobCount = mobs.size(); // 仅更新统计，不输出日志
    return mobs;
}

static void parallelProcessMobAI(Level& level) {
    std::vector<Actor*> mobs = collectAllMobs(level);
    if (mobs.empty()) {
        return; // 没有生物，直接返回（不输出日志）
    }

    const size_t numThreads = std::max(1u, std::thread::hardware_concurrency());
    const size_t mobsPerThread = (mobs.size() + numThreads - 1) / numThreads;

    std::vector<std::future<WorkerResult>> futures;
    futures.reserve(numThreads);

    for (size_t t = 0; t < numThreads; ++t) {
        size_t startIdx = t * mobsPerThread;
        size_t endIdx = std::min(startIdx + mobsPerThread, mobs.size());
        if (startIdx >= mobs.size()) break;

        futures.push_back(std::async(std::launch::async, [&mobs, startIdx, endIdx]() {
            return workerProcessMobRange(mobs.begin() + startIdx, mobs.begin() + endIdx);
        }));
    }

    size_t tickProcessed = 0;
    for (auto& future : futures) {
        try {
            auto result = future.get();
            tickProcessed += result.processed;
        } catch (const std::exception& e) {
            getLogger().error("Future get exception: {}", e.what());
        } catch (...) {
            getLogger().error("Future get unknown exception");
        }
    }

    totalProcessed += tickProcessed;
    totalTicks++;
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
    getLogger().info("Loaded. enabled={}, debug={}", config.enabled, config.debug);
    return true;
}

bool Optimizer::enable() {
    if (config.debug) startDebugTask();
    getLogger().info("Enabled.");
    return true;
}

bool Optimizer::disable() {
    stopDebugTask();
    resetStats();
    {
        std::lock_guard<std::mutex> lock(crashedIdsMutex);
        crashedIds.clear();
    }
    getLogger().info("Disabled");
    return true;
}

} // namespace mob_ai_optimizer

// 钩子
LL_AUTO_TYPE_INSTANCE_HOOK(
    MobAiStepHook,
    ll::memory::HookPriority::Normal,
    Mob,
    &Mob::$aiStep,
    void
) {
    using namespace mob_ai_optimizer;
    // 修改点2：优化启用且不是玩家 -> 跳过原 aiStep（因为会在 LevelTick 中并行处理）
    if (config.enabled && !this->isPlayer()) {
        return;
    }
    // 否则（未启用优化 或 是玩家）执行原函数
    origin();
}

LL_AUTO_TYPE_INSTANCE_HOOK(
    LevelTickHook,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::$tick,
    void
) {
    using namespace mob_ai_optimizer;

    if (config.enabled) {
        parallelProcessMobAI(*this);
    }

    origin();
}

LL_REGISTER_MOD(mob_ai_optimizer::Optimizer, mob_ai_optimizer::Optimizer::getInstance());
