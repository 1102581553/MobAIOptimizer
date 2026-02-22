#pragma once

#include <ll/api/mod/NativeMod.h>
#include <ll/api/event/Listener.h>          // 用于 ListenerPtr
#include <mc/legacy/ActorUniqueID.h>
#include <unordered_map>

namespace mob_ai_optimizer {

class Optimizer {
public:
    static Optimizer& getInstance();

    Optimizer() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();

private:
    ll::mod::NativeMod& mSelf;
    ll::event::ListenerPtr mListener;        // 保存事件监听器句柄
};

// 全局数据
extern std::unordered_map<ActorUniqueID, int> lastAiTick;
extern int processedThisTick;
extern int currentTickId;

// 配置常量
constexpr int COOLDOWN_TICKS = 5;   // 每个生物至少间隔5 tick执行AI
constexpr int MAX_PER_TICK   = 50;  // 每tick最多处理50个生物

} // namespace mob_ai_optimizer
