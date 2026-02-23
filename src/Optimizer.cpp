#include "Optimizer.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/mod/RegisterHelper.h>
#include <ll/api/io/Logger.h>
#include <ll/api/io/LoggerRegistry.h>
#include <mc/world/actor/Mob.h>
#include <mc/world/actor/Actor.h>
#include <mc/world/level/Level.h>
#include <mc/world/level/Tick.h>
#include <mc/legacy/ActorUniqueID.h>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <future>
#include <chrono>
#include <algorithm>  // ← 新增：用于 std::remove_if

namespace mob_ai_optimizer {

// ====================== 全局变量定义 ======================
std::unordered_map<ActorUniqueID, std::uint64_t> lastAiTick;
std::mutex lastAiTickMutex;
std::atomic<int> processedThisTick{0};
std::atomic<std::uint64_t> currentTickId{0};
std::atomic<int> cleanupCounter{0};

std::vector<std::future<void>> cleanupTasks;
std::mutex cleanupTasksMutex;
std::atomic<bool> stopping{false};

// Logger 获取（线程安全初始化）
static std::shared_ptr<ll::io::Logger> logger;

ll::io::Logger& getLogger() {
    static std::once_flag flag;
    static std::shared_ptr<ll::io::Logger> instance;
    std::call_once(flag, []{
        instance = ll::io::LoggerRegistry::getInstance()
                      .getOrCreate("MobAIOptimizer");
    });
    return *instance;
}

// ====================== 辅助函数：清理已完成的 future ======================
// ⚠️ 调用前必须持有 cleanupTasksMutex 锁
inline void pruneCompletedFutures() {
    cleanupTasks.erase(
        std::remove_if(cleanupTasks.begin(), cleanupTasks.end(),
            [](std::future<void>& fut) {
                // wait_for(0) 非阻塞检查任务是否完成
                return fut.valid() && 
                       fut.wait_for(std::chrono::seconds(0)) 
                           == std::future_status::ready;
            }),
        cleanupTasks.end()
    );
}

// ====================== 异步清理函数 ======================
void performCleanupAsync(std::uint64_t currentTick) {
    if (stopping.load()) return;

    auto fut = std::async(std::launch::async, [currentTick] {
        try {
            std::lock_guard<std::mutex> lock(lastAiTickMutex);
            for (auto it = lastAiTick.begin(); it != lastAiTick.end(); ) {
                if (currentTick - it->second > MAX_EXPIRED_AGE) {
                    it = lastAiTick.erase(it);
                } else {
                    ++it;
                }
            }
        } catch (const std::exception& e) {
            getLogger().error("后台清理任务异常: {}", e.what());
        } catch (...) {
            getLogger().error("后台清理任务未知异常");
        }
    });

    {
        std::lock_guard<std::mutex> lock(cleanupTasksMutex);
        // ✨ 修复：添加前先清理已完成的任务，防止 vector 无限增长
        pruneCompletedFutures();
        cleanupTasks.push_back(std::move(fut));
    }
}

// ====================== 插件生命周期 ======================
Optimizer& Optimizer::getInstance() {
    static Optimizer instance;
    return instance;
}

bool Optimizer::load() {
    getLogger().info("生物AI优化插件已加载");
    {
        std::lock_guard<std::mutex> lock(lastAiTickMutex);
        lastAiTick.reserve(INITIAL_MAP_RESERVE);
    }
    return true;
}

bool Optimizer::enable() {
    getLogger().info("生物AI优化插件已启用");
    return true;
}

bool Optimizer::disable() {
    getLogger().info("生物AI优化插件正在禁用...");

    stopping.store(true);

    // 等待所有后台清理任务完成
    {
        std::lock_guard<std::mutex> lock(cleanupTasksMutex);
        // ✨ 修复：先等待所有 future 完成，再清理 vector
        for (auto& fut : cleanupTasks) {
            if (fut.valid()) {
                fut.wait();  // 阻塞直到任务完成
            }
        }
        cleanupTasks.clear();  // 清空已完成的 future
    }

    {
        std::lock_guard<std::mutex> lock(lastAiTickMutex);
        lastAiTick.clear();
    }

    getLogger().info("生物AI优化插件已禁用");
    return true;
}

} // namespace mob_ai_optimizer

// ====================== AI优化 Hook ======================
LL_AUTO_TYPE_INSTANCE_HOOK(
    MobAiStepHook,
    ll::memory::HookPriority::Normal,
    Mob,
    &Mob::$aiStep,
    void
) {
    using namespace mob_ai_optimizer;

    if (stopping.load()) {
        origin();
        return;
    }

    std::uint64_t currentTick = this->getLevel().getCurrentServerTick().tickID;

    // ---------- 跨 tick 重置（原子操作保证单线程执行） ----------
    uint64_t expected = currentTickId.load(std::memory_order_acquire);
    if (currentTick != expected &&
        currentTickId.compare_exchange_strong(expected, currentTick,
                                              std::memory_order_release,
                                              std::memory_order_relaxed)) {
        processedThisTick.store(0, std::memory_order_relaxed);
        
        int oldCleanup = ++cleanupCounter;
        if (oldCleanup >= CLEANUP_INTERVAL_TICKS) {
            performCleanupAsync(currentTick);
            cleanupCounter = 0;
        }
        
        // ✨ 优化：tick 重置时也顺便清理已完成的 future（低频开销）
        {
            std::lock_guard<std::mutex> lock(cleanupTasksMutex);
            pruneCompletedFutures();
        }
    }

    // ---------- 检查冷却 ----------
    ActorUniqueID id = this->getOrCreateUniqueID();
    {
        std::lock_guard<std::mutex> lock(lastAiTickMutex);
        auto it = lastAiTick.find(id);
        if (it != lastAiTick.end() && currentTick - it->second < COOLDOWN_TICKS) {
            return; // 还在冷却期，跳过本次 AI
        }
    }

    // ---------- 每 tick 限流（原子递增，超额回退）----------
    int old = processedThisTick.fetch_add(1, std::memory_order_relaxed);
    if (old >= MAX_PER_TICK) {
        processedThisTick.fetch_sub(1, std::memory_order_relaxed);
        return;
    }

    origin();

    {
        std::lock_guard<std::mutex> lock(lastAiTickMutex);
        lastAiTick[id] = currentTick;
    }
}

// ====================== 自动清理 Hook ======================
LL_AUTO_TYPE_INSTANCE_HOOK(
    ActorDespawnHook,
    ll::memory::HookPriority::Normal,
    Actor,
    &Actor::$despawn,
    void
) {
    using namespace mob_ai_optimizer;
    {
        std::lock_guard<std::mutex> lock(lastAiTickMutex);
        lastAiTick.erase(this->getOrCreateUniqueID());
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
    {
        std::lock_guard<std::mutex> lock(lastAiTickMutex);
        lastAiTick.erase(this->getOrCreateUniqueID());
    }
    origin();
}

// ====================== 注册插件 ======================
LL_REGISTER_MOD(mob_ai_optimizer::Optimizer, mob_ai_optimizer::Optimizer::getInstance());
