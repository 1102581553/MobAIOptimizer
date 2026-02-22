#pragma once

#include <ll/api/plugin/Plugin.h>
#include <ll/api/event/Listener.h>
#include <mc/legacy/ActorUniqueID.h>
#include <unordered_map>

namespace mob_ai_optimizer {

class Optimizer : public ll::plugin::Plugin {
public:
    static Optimizer& getInstance();

    Optimizer() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool onLoad() override;
    bool onEnable() override;
    bool onDisable() override;

private:
    ll::event::ListenerPtr mRemoveListener;
    ll::mod::NativeMod& mSelf;
};

// 全局数据
extern std::unordered_map<ActorUniqueID, int> lastAiTick;
extern int processedThisTick;
extern int currentTickId;

// 配置常量
constexpr int COOLDOWN_TICKS = 5;   // 每个生物至少间隔5 tick执行AI
constexpr int MAX_PER_TICK   = 50;  // 每tick最多处理50个生物

} // namespace mob_ai_optimizer
