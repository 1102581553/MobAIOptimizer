#include "Optimizer.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/mod/RegisterHelper.h>
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
#include <ll/api/Logger.h>   // 用于异常日志

namespace mob_ai_optimizer {

// 全局变量定义
std::unordered_map<ActorUniqueID, std::uint64_t> lastAiTick;
std::mutex lastAiTickMutex;
std::atomic<int> processedThisTick{0};
std::atomic<std::uint64_t> currentTickId{0};
std::atomic<int> cleanupCounter{0};

std::vector<std::future<void>> cleanupTasks;
std::mutex cleanupTasksMutex;
std::atomic<bool> stopping{false};

// ====================== 异步清理函数 ======================
void performCleanupAsync(std::uint64_t currentTick) {
    if (stopping.load()) return;   // 插件正在卸载，不再启动新任务

    // 使用 std::async 启动后台任务
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
            // 异常处理，防止 future 析构时崩溃
            ll::Logger("MobAIOptimizer").error("后台清理任务异常: {}", e.what());
        } catch (...) {
            ll::Logger("MobAIOptimizer").error("后台清理任务未知异常");
        }
    });

    // 将 future 存入全局容器（需加锁）
    {
        std::lock_guard<std::mutex> lock(cleanupTasksMutex);
        cleanupTasks.push_back(std::move(fut));
    }

    // 可选：定期清理已完成的 future（避免容器无限增长）
    // 此处为了简洁，在 disable 时统一清理
}

// ====================== 插件生命周期 ======================
Optimizer& Optimizer::getInstance() {
    static Optimizer instance;
    return instance;
}

bool Optimizer::load() {
    getSelf().getLogger().info("生物AI优化插件已加载");
    {
        std::lock_guard<std::mutex> lock(lastAiTickMutex);
        lastAiTick.reserve(INITIAL_MAP_RESERVE);
    }
    return true;
}

bool Optimizer::enable() {
    getSelf().getLogger().info("生物AI优化插件已启用");
    return true;
}

bool Optimizer::disable() {
    getSelf().getLogger().info("生物AI优化插件正在禁用...");

    // 1. 设置停止标志，禁止启动新任务
    stopping.store(true);

    // 2. 等待所有后台清理任务完成
    {
        std::lock_guard<std::mutex> lock(cleanupTasksMutex);
        for (auto& fut : cleanupTasks) {
            if (fut.valid()) {
                fut.wait();   // 等待任务完成（可能会抛出异常，但我们已在任务内捕获）
            }
        }
        cleanupTasks.clear(); // 清空容器
    }

    // 3. 清理主数据（此时无后台任务访问 lastAiTick）
    {
        std::lock_guard<std::mutex> lock(lastAiTickMutex);
        lastAiTick.clear();
    }

    getSelf().getLogger().info("生物AI优化插件已禁用");
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

    // 如果插件正在停止，直接放行（不优化）
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
        // 只有成功更新 currentTickId 的线程执行重置
        processedThisTick.store(0, std::memory_order_relaxed);
        int oldCleanup = ++cleanupCounter;
        if (oldCleanup >= CLEANUP_INTERVAL_TICKS) {
            performCleanupAsync(currentTick);   // 异步清理
            cleanupCounter = 0;
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
        // 已超额，回退并返回
        processedThisTick.fetch_sub(1, std::memory_order_relaxed);
        return;
    }

    // 执行原始 AI
    origin();

    // AI 成功执行后，更新最后执行 tick
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
