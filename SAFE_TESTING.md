# 真机安全测试指南

本项目默认面向低入侵测试。`grant.conf` 中的 `control_mode` 控制 sysmodule 触碰 PCTL 的强度：

- `disabled`：只保留日志与 fail-open 行为，不处理授权请求，不调用 PCTL。
- `observe`：验证离线码并读取 PCTL 状态，返回 `dry_run:true`，不写 PCTL，不消费 nonce。
- `grant`：仅在有效离线码请求时写入当天限制，不在开机时启用 play timer。
- `enforce`：强控制模式，会在开机时尝试确保 play timer 启用，并在授权写入后刷新 play timer。

## 恢复开关

只要 SD 卡存在以下文件，sysmodule 会进入 fail-open 状态，不触碰 PCTL：

```text
sdmc:/switch/pctltcp-sysmodule/disable.flag
```

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

因此无限制或未配置的当天限制不会被授权码自动改成有限制。若确实要允许这种转换，必须显式设置：

```json
{
    "allow_unlimited_to_limited": true
}
```
