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
