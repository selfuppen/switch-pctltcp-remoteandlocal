# 真机安全测试指南

本项目默认面向低入侵测试。`grant.conf` 中的 `control_mode` 控制 sysmodule 触碰 PCTL 的强度：

更详细的真机分阶段流程见 `REAL_SWITCH_TESTING.md`。

- `disabled`：只保留日志与 fail-open 行为，不处理授权请求，不调用 PCTL。
- `observe`：验证离线码并读取 PCTL 状态，返回 `dry_run:true`，不写 PCTL，不消费 nonce。
- `grant`：仅在有效离线码请求时写入当天限制，不在开机时启用 play timer。
- `enforce`：强控制模式，会在开机时尝试确保 play timer 启用，并在授权写入后刷新 play timer。

## 恢复开关

只要 SD 卡存在以下文件，sysmodule 会进入 fail-open 状态，不触碰 PCTL：

```text
sdmc:/switch/pctltcp-sysmodule/disable.flag
```

这里的 fail-open 指“异常或手动停用时默认放行”。在本项目里，它表示 sysmodule 不再处理离线授权请求、不读取或写入 PCTL、不尝试启用 play timer，只保留可恢复的停用状态和日志。它的目的不是加强限制，而是在真机测试遇到异常时尽量避免继续影响系统家长控制。

如果需要彻底停止 boot2 自启，删除：

```text
sdmc:/atmosphere/contents/010000000000BD23/flags/boot2.flag
```

## 推荐真机流程

1. 先只测试 companion NRO，不放 `boot2.flag`。
2. 需要测试 sysmodule 启动时，用 `./tools/package_sdmc.sh --boot2 --control-mode observe` 打包。
3. 在 `observe` 模式输入有效码，确认结果中有 `dry_run:true`，且系统家长控制没有变化。
4. 手动在系统家长控制中设置明确的今日限制，例如 120 分钟，再切到 `grant`，用 1 分钟码做最小写入测试。
5. 确认 `last_pctl_backup.dat` 已写入，再扩大测试有效码、重复码、错日期码、错密钥码和超上限码。
6. 只有稳定后才使用 `./tools/package_sdmc.sh --strong-control` 或手动设置 `control_mode=enforce`。

## 家长区和本地规则测试

companion 主屏不会直接显示家长设置入口。需要长按 `L + R + X` 约 2 秒，再输入 `settings.conf` 中的本地设置密码进入家长区。默认密码仍为 `1234`。

家长区会写入或读取以下文件：

```text
sdmc:/switch/pctltcp-sysmodule/time_rules.json
sdmc:/switch/pctltcp-sysmodule/time_state.json
sdmc:/switch/pctltcp-sysmodule/events.jsonl
sdmc:/switch/pctltcp-sysmodule/monthly_report.txt
```

建议先在 `observe` 模式测试家长区操作：今日固定额度、+15/+30/+60、今日不限、恢复周模板、平日/周末模板、bedtime 和 parent unlock。`observe` 模式应返回 `dry_run:true`，并只更新本地规则/事件，不应改变系统家长控制。

## raw 0 今日禁玩

`raw 0` 禁玩是高风险真机验证项。默认 `time_rules.json` 中：

```json
{
    "raw_block_verified": false
}
```

在该状态下，普通“今日禁止游玩”请求会返回 `raw_block_not_verified`，不会写 PCTL。需要先在家长区触发 raw block probe，在真实 Switch 上确认：

- `last_pctl_backup.dat` 已生成。
- PCTL 原始当天分钟数写为 `0`。
- 达到限制或启动游戏时的系统行为符合预期。
- 可以通过恢复周模板或备份手动恢复。

确认后，再在家长区按提示标记 `raw_block_verified=true`，之后才允许使用“今日禁止游玩”。

## 安全默认

示例配置默认：

```json
{
    "control_mode": "observe",
    "allow_unlimited_to_limited": false
}
```

`allow_unlimited_to_limited` 控制是否允许把“今天原本无限制/未配置”的状态改成“今天有明确分钟限制”。默认值 `false` 是安全测试取向：如果今天没有明确限制，授权码不会把它改成有限制，而是返回 `unlimited_not_allowed`。真机写入测试时，建议先手动在系统家长控制里设置今天的明确限制，例如 120 分钟，再用授权码做加时验证。

若确实要允许从无限制/未配置状态转换为有限制，必须显式设置：

```json
{
    "allow_unlimited_to_limited": true
}
```

此时如果今天原本没有限制，sysmodule 会把当前值按 0 分钟理解，再叠加授权码分钟数。例如输入 30 分钟码，今天会被写成 30 分钟限制。
