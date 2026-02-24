
# MobAIOptimizer

一个基于 LeviLamina 框架的 Minecraft 基岩版服务端插件，用于智能优化生物 AI 计算性能。

## 功能特性

- **智能节流**：动态限制每 tick 处理的 AI 实体数量，避免服务器卡顿
- **自适应调节**：根据服务器 tick 耗时自动调整优化参数
- **冷却机制**：为已处理的生物设置冷却期，避免重复计算
- **内存管理**：自动清理已卸载或过期实体的追踪数据
- **调试统计**：提供详细的性能统计信息（处理数、跳过数、清理数等）

## 工作原理

插件通过 Hook `Mob::aiStep` 函数来拦截生物 AI 更新：

1. **Tick 限流**：每 tick 最多处理 `dynMaxPerTick` 个生物的 AI
2. **冷却期**：每个生物在处理后进入 `dynCooldownTicks` 的冷却期
3. **动态调整**：根据 Level tick 耗时自动调节 `dynMaxPerTick` 和 `dynCooldownTicks`
4. **数据清理**：定期清理过期记录，实体 despawn/remove 时即时移除追踪

## 配置文件

位于 `config/MobAIOptimizer/config.json`：

```json
{
    "version": 2,
    "enabled": true,
    "debug": false,
    "targetTickMs": 50,
    "maxPerTickStep": 4,
    "cooldownTicksStep": 1,
    "cleanupIntervalTicks": 100,
    "maxExpiredAge": 600,
    "initialMapReserve": 1000
}
```

### 配置项说明

| 配置项 | 说明 | 默认值 |
|--------|------|--------|
| `enabled` | 是否启用优化 | `true` |
| `debug` | 是否启用调试日志（每5秒输出统计） | `false` |
| `targetTickMs` | 目标 tick 耗时（毫秒），超过则收紧限制 | `50` |
| `maxPerTickStep` | 每 tick 处理数量调整步长 | `4` |
| `cooldownTicksStep` | 冷却 tick 数调整步长 | `1` |
| `cleanupIntervalTicks` | 过期数据清理间隔（ticks） | `100` |
| `maxExpiredAge` | 数据过期时间（ticks） | `600` |
| `initialMapReserve` | 追踪表初始预留大小 | `1000` |

## 动态参数算法

- **初始值**：
  - `dynMaxPerTick` = `maxPerTickStep` × 10
  - `dynCooldownTicks` = `cooldownTicksStep` × 4

- **自适应逻辑**（每个 Level tick 后）：
  - 若 tick 耗时 > `targetTickMs`：减少处理量，增加冷却期
  - 若 tick 耗时 ≤ `targetTickMs`：增加处理量，减少冷却期

**统计字段说明**：
- `processed`：实际处理的 AI 次数
- `cooldownSkip`：因处于冷却期而跳过的次数
- `throttleSkip`：因达到 tick 上限而跳过的次数
- `skipRate`：总跳过率（越高说明优化效果越明显）
- `despawnClean`：因实体卸载而清理的记录数
- `expiredClean`：因过期而清理的记录数
- `tracked`：当前追踪的实体数量


## 许可证

[MIT License](LICENSE)

