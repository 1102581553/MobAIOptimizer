#include "Optimizer.h"
#include <ll/api/memory/Hook.h>
#include <ll/api/mod/RegisterHelper.h>
#include <ll/api/io/FileUtils.h>
#include <nlohmann/json.hpp>
#include <filesystem>

namespace mob_ai_optimizer {

Config config{};
std::unordered_map<ActorUniqueID, int> lastAiTick;
int processedThisTick = 0;
int skippedThisTick = 0;
int currentTickId = -1;

Optimizer& Optimizer::getInstance() {
    static Optimizer instance;
    return instance;
}

bool Optimizer::load() {
    getLogger().info("生物AI优化插件已加载");
    return true;
}

void Optimizer::loadConfig() {
    auto configDir = getSelf().getConfigDir();           // 标准配置目录
    ll::io::ensureDir(configDir);
    auto configPath = configDir / "config.json";

    // 不存在则生成默认配置
    if (!std::filesystem::exists(configPath)) {
        nlohmann::json j = {
            {"cooldownTicks", config.cooldownTicks},
            {"maxPerTick",    config.maxPerTick},
            {"debug",         config.debug}
        };
        if (ll::io::writeFile(configPath, j.dump(4))) {
            getLogger().info("已生成默认配置文件 → {}", configPath.string());
        }
    }

    // 读取配置
    try {
        auto content = ll::io::readFile(configPath);
        auto j = nlohmann::json::parse(content);

        config.cooldownTicks = j.value("cooldownTicks", 5);
        config.maxPerTick    = j.value("maxPerTick", 50);
        config.debug         = j.value("debug", false);

        getLogger().info("配置文件加载成功 | 冷却:{} tick | 每tick上限:{} | Debug: {}",
            config.cooldownTicks, config.maxPerTick, config.debug ? "已开启" : "已关闭");
    } catch (const std::exception& e) {
        getLogger().error("配置文件读取失败: {}", e.what());
        getLogger().info("使用默认配置");
    }
}

bool Optimizer::enable() {
    loadConfig();                                     // 加载配置
    getLogger().info("§a生物AI优化插件已启用");
    return true;
}

bool Optimizer::disable() {
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
    auto& logger = Optimizer::getInstance().getLogger();

    auto* self = this;
    int tickInt = static_cast<int>(self->getLevel().getCurrentServerTick().tickID);

    // 新的一 tick → 输出上一 tick 的优化统计
    if (tickInt != currentTickId) {
        if (config.debug && (processedThisTick > 0 || skippedThisTick > 0)) {
            logger.debug("§b[AI Optimizer] Tick {} | 处理:{} | 跳过:{} | 缓存:{} 个生物",
                currentTickId, processedThisTick, skippedThisTick, lastAiTick.size());
        }
        currentTickId = tickInt;
        processedThisTick = 0;
        skippedThisTick = 0;
    }

    ActorUniqueID id = self->getOrCreateUniqueID();

    // 冷却跳过
    auto it = lastAiTick.find(id);
    if (it != lastAiTick.end() && tickInt - it->second < config.cooldownTicks) {
        skippedThisTick++;
        return;
    }

    // 每tick上限跳过
    if (processedThisTick >= config.maxPerTick) {
        skippedThisTick++;
        return;
    }

    // 执行AI
    processedThisTick++;
    lastAiTick[id] = tickInt;
    origin();
}

// ====================== 自动清理 Hook ======================
LL_AUTO_TYPE_INSTANCE_HOOK(
    ActorDespawnHook,
    ll::memory::HookPriority::Normal,
    Actor,
    &Actor::$despawn,
    void
) {
    mob_ai_optimizer::lastAiTick.erase(this->getOrCreateUniqueID());
    origin();
}

LL_REGISTER_MOD(mob_ai_optimizer::Optimizer, mob_ai_optimizer::Optimizer::getInstance());
