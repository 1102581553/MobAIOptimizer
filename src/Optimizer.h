#pragma once
#include <ll/api/Config.h>
#include <ll/api/mod/NativeMod.h>

namespace mob_ai_optimizer {

struct Config {
    int  version = 5;   // 配置版本
    bool enabled = true;
    bool debug   = false;
};

Config& getConfig();
bool    loadConfig();
bool    saveConfig();

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
};

} // namespace mob_ai_optimizer
