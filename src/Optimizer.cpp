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
#include <windows.h>  // 用于 SEH 异常处理

namespace mob_ai_optimizer {

static Config config;
static std::shared_ptr<ll::io::Logger> log;
static bool debugTaskRunning = false;

// 调试统计
static size_t totalProcessed = 0;
static size_t totalTicks = 0;
static size_t lastMobCount = 0;

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

        // 使用 SEH 捕获所有异常（包括访问违例和 C++ 异常）
        __try {
            mob->aiStep();
            result.processed++;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            DWORD code = GetExceptionCode();
            getLogger().error("SEH exception in aiStep for mob {}: code 0x{:X}", (uint64_t)mob, code);
        }
    }

    return result;
}

static std::vector<Actor*> collectAllMobs(Level& level) {
    std::vector<Actor*> mobs;
    auto actors = level.getRuntimeActorList();
    
    getLogger().info("getRuntimeActorList returned {} actors", actors.size());

    for (Actor* actor : actors) {
        if (!actor) continue;
        if (actor->hasCategory(::ActorCategory::Mob)) {
            getLogger().info("Found Mob: {}", (uint64_t)actor);
            mobs.push_back(actor);
        }
    }

    getLogger().info("collectAllMobs found {} mobs", mobs.size());
    lastMobCount = mobs.size();
    return mobs;
}

static void parallelProcessMobAI(Level& level) {
    std::vector<Actor*> mobs = collectAllMobs(level);
    if (mobs.empty()) {
        getLogger().info("No mobs to process");
        return;
    }

    const size_t numThreads = std::max(1u, std::thread::hardware_concurrency());
    const size_t mobsPerThread = (mobs.size() + numThreads - 1) / numThreads;

    getLogger().info("Processing {} mobs with {} threads", mobs.size(), numThreads);

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

    getLogger().info("Processed {} mobs this tick", tickProcessed);
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
    if (config.enabled) {
        return;
    }
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
