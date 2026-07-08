# 真机 control_mode 测试指南

本文档用于真机验证 `grant.conf` 中 `control_mode` 的行为差异，并给出从低风险到强控制的测试步骤。模拟器不能完整验证 PCTL/sysmodule 行为，下面的步骤需要真实 Nintendo Switch、Atmosphere CFW、可读写的 SD 卡，以及已构建完成的 `pctltcp-sysmodule.nsp` 和 `companion/pctltcp-grant.nro`。

## control_mode 说明

`control_mode` 控制 sysmodule 对 PCTL 家长控制服务的触碰强度。配置路径为：

```text
sdmc:/switch/pctltcp-sysmodule/grant.conf
```

推荐从 `disabled` 或 `observe` 开始，确认日志和前台 NRO 正常后，再切到 `grant`，最后才测试 `enforce`。

| 模式 | 授权请求行为 | PCTL 读取 | PCTL 写入 | 开机 play timer 行为 | nonce 消耗 |
| --- | --- | --- | --- | --- | --- |
| `disabled` | 请求会被拒绝，返回 `reason:"disabled"` | 不读取 | 不写入 | 不启用 | 不消耗 |
| `observe` | 验证离线码并计算预期加时，返回 `dry_run:true` | 读取 | 不写入 | 不启用，只记录状态 | 不消耗 |
| `grant` | 有效离线码会写入当天限制 | 读取 | 写入当天限制 | 不主动启用，只记录状态 | 成功写入后消耗 |
| `enforce` | 行为等同 `grant`，并加强 play timer 启用 | 读取 | 写入当天限制 | 开机尝试启用 play timer，写入后刷新 play timer | 成功写入后消耗 |

补充说明：

- 无效或未知的 `control_mode` 会按 `observe` 处理。
- `disable.flag` 的优先级高于 `control_mode`。只要存在 `sdmc:/switch/pctltcp-sysmodule/disable.flag`，sysmodule 会进入 fail-open 状态并拒绝授权请求。
- `observe` 仍会校验签名、日期、分钟上限和重复记录；但验证成功时不会写 PCTL，也不会把 nonce 写入 `used_grants.dat`。
- `grant` 和 `enforce` 写入前会先创建 `last_pctl_backup.dat`。写入成功后，同一日期和 nonce 会记录到 `used_grants.dat`，再次输入同一个码应返回 `used_token`。
- 当 `allow_unlimited_to_limited` 为 `false` 时，如果当天本来是无限制或未配置状态，`observe`、`grant` 和 `enforce` 都会拒绝加时并返回 `unlimited_not_allowed`。真机写入测试前建议先在系统家长控制里手动设置明确的当天限制，例如 120 分钟。

## fail-open 和无限制保护

fail-open 可以理解为“出问题或手动停用时默认放行”。在本项目中，创建 `disable.flag` 后，sysmodule 会停止触碰 PCTL：不处理离线授权请求、不读取或写入家长控制设置、不尝试启用或刷新 play timer。这个机制用于真机测试时快速恢复，避免模块在异常情况下继续改变系统状态。

`allow_unlimited_to_limited` 是另一层保护，处理的是“今天原本没有限制”的情况：

- `false`：默认值。今天无限制或未配置时，不允许授权码把今天改成有限制，结果会返回 `unlimited_not_allowed`。这是推荐的安全测试设置。
- `true`：允许从无限制/未配置转换为有限制。当前实现会把原始限制当作 0 分钟，再叠加授权码分钟数；例如 30 分钟码会把今天写成 30 分钟限制。

因此，“加时”测试最好先手动设置一个明确的当天限制，比如 120 分钟，再输入 1 分钟码，预期新限制变成 121 分钟。只有明确要验证从无限制切到有限制时，才把 `allow_unlimited_to_limited` 改为 `true`。

## 恢复和停用

测试前先确认你知道如何停用模块。

临时 fail-open：

```text
sdmc:/switch/pctltcp-sysmodule/disable.flag
```

彻底停止 boot2 自启：

```text
删除 sdmc:/atmosphere/contents/010000000000BD23/flags/boot2.flag
```

建议测试时保留电脑读卡器或其他可编辑 SD 卡的方式。一旦出现异常，关机取出 SD 卡，创建 `disable.flag` 或删除 `boot2.flag` 后再开机。

## 测试前准备

1. 运行构建：

```bash
make
```

2. 准备测试用配置。不要使用真实家庭长期密钥，先用专门的测试密钥。可以把下面内容保存为仓库根目录的 `grant.conf` 后再打包，也可以打包后编辑 SD 卡上的 `sdmc:/switch/pctltcp-sysmodule/grant.conf`：

```json
{
    "device_id": "kid-switch-test",
    "grant_secret": "change-me-test-secret-at-least-32-bytes",
    "max_add_minutes": 10,
    "control_mode": "observe",
    "allow_unlimited_to_limited": false
}
```

3. 在 Switch 系统家长控制里手动设置一个明确的当天限制，例如 120 分钟。这样可以避免安全默认值触发 `unlimited_not_allowed`。
4. 清理上一轮测试残留，避免误判：

```text
sdmc:/switch/pctltcp-sysmodule/grant_request.json
sdmc:/switch/pctltcp-sysmodule/grant_result.json
sdmc:/switch/pctltcp-sysmodule/disable.flag
```

5. 如果要重复验证同一个 nonce，先删除旧的 `used_grants.dat`。只在测试环境这样做：

```text
sdmc:/switch/pctltcp-sysmodule/used_grants.dat
```

## 生成测试码

当天有效码：

```bash
python tools/grant_code.py \
  --minutes 1 \
  --device kid-switch-test \
  --secret change-me-test-secret-at-least-32-bytes
```

超过上限码，配合 `max_add_minutes: 10`：

```bash
python tools/grant_code.py \
  --minutes 11 \
  --device kid-switch-test \
  --secret change-me-test-secret-at-least-32-bytes
```

错日期码：

```bash
python tools/grant_code.py \
  --minutes 1 \
  --device kid-switch-test \
  --secret change-me-test-secret-at-least-32-bytes \
  --date 2026-07-06 \
  --nonce 0x1234
```

错密钥码：

```bash
python tools/grant_code.py \
  --minutes 1 \
  --device kid-switch-test \
  --secret wrong-test-secret
```

注意：错日期测试必须使用非当天日期。当前日期不同的时候，请把 `--date` 改成任意非当天的 `YYYY-MM-DD`。

## 阶段 1：只测 companion NRO

目的：确认前台输入、设置密码和 SD 卡文件交互正常，不启动 boot2 sysmodule。

1. 打安全测试包，不包含 `boot2.flag`：

```bash
./tools/package_sdmc.sh --safe-test
```

2. 将 zip 解压到 SD 卡根目录。
3. 确认不存在：

```text
sdmc:/atmosphere/contents/010000000000BD23/flags/boot2.flag
```

4. 进入 Homebrew Menu，启动 `pctltcp-grant.nro`。
5. 按 `X` 进入设置，默认密码 `1234` 应可进入；修改密码后旧密码应失败，新密码应成功。
6. 按 `A` 输入任意格式正确的离线码。
7. 检查是否生成：

```text
sdmc:/switch/pctltcp-sysmodule/grant_request.json
```

预期：

- NRO 可启动，不崩溃。
- 设置密码可读写 `settings.conf`。
- 输入离线码会写出 `grant_request.json`。
- 因为没有后台 sysmodule，等待结果可能超时，这是预期行为。

## 阶段 2：disabled 模式启动冒烟

目的：确认 boot2 sysmodule 能启动、写日志，并且不触碰 PCTL。

1. 打包：

```bash
./tools/package_sdmc.sh --boot2 --control-mode disabled
```

2. 解压到 SD 卡根目录，重启进入 CFW。
3. 检查日志：

```text
sdmc:/switch/pctltcp-sysmodule/sysmodule.log
```

预期包含：

```text
pctltcp-sysmodule starting (offline grant mode)
grant: config loaded (mode=disabled
grant: fail-open disabled state active
PCTL status skipped because control is disabled
Skipping boot play timer enforcement (mode=disabled, disabled=true).
```

4. 打开 NRO，输入一个当天有效码。
5. 检查：

```text
sdmc:/switch/pctltcp-sysmodule/grant_result.json
```

预期类似：

```json
{"status":"error","reason":"disabled","mode":"disabled"}
```

通过标准：

- sysmodule 自动启动并写日志。
- 授权请求被明确拒绝为 `disabled`。
- 系统家长控制时间没有变化。

## 阶段 3：observe 模式验证

目的：验证离线码、日期、签名、PCTL 读取和结果回写，但不修改系统家长控制。

1. 打包：

```bash
./tools/package_sdmc.sh --boot2 --control-mode observe --max-add-minutes 10 --no-allow-unlimited-to-limited
```

2. 解压到 SD 卡根目录，确认 `grant.conf` 中的 `device_id` 和 `grant_secret` 与生成测试码的参数一致。
3. 重启进入 CFW。
4. 检查 `sysmodule.log`，预期包含：

```text
grant: config loaded (mode=observe
boot PCTL status: mode=observe
Skipping boot play timer enforcement (mode=observe, disabled=false).
```

5. 打开 NRO，输入当天有效的 1 分钟码。
6. 检查 `grant_result.json`，预期类似：

```json
{"status":"ok","applied_minutes":1,"today_limit":121,"dry_run":true,"mode":"observe"}
```

7. 检查系统家长控制中的当天限制，应仍是原来的 120 分钟，没有被改成 121 分钟。
8. 再次输入同一个码。因为 `observe` 不消耗 nonce，如果没有在其他模式成功使用过该码，应仍然返回 `ok` 和 `dry_run:true`。

异常分支：

- 如果返回 `unlimited_not_allowed`，说明当天没有明确限制，或 `allow_unlimited_to_limited` 为 `false` 且系统状态被识别为无限制。先在系统家长控制中手动设置当天限制后再测。
- 如果返回 `wrong_date`，检查 Switch 日期和测试码 `--date`。
- 如果返回 `bad_signature`，检查 `grant_secret` 和 `device_id`。

## 阶段 4：grant 模式最小写入

目的：验证有效码真实写入当天限制，但不开机强制启用 play timer。

1. 在系统家长控制中确认当天限制为明确值，例如 120 分钟。
2. 清理或备份旧结果文件：

```text
sdmc:/switch/pctltcp-sysmodule/grant_result.json
sdmc:/switch/pctltcp-sysmodule/last_pctl_backup.dat
```

3. 打包：

```bash
./tools/package_sdmc.sh --boot2 --control-mode grant --max-add-minutes 10 --no-allow-unlimited-to-limited
```

4. 解压到 SD 卡根目录，重启进入 CFW。
5. 打开 NRO，输入当天有效的 1 分钟码。
6. 检查 `grant_result.json`，预期类似：

```json
{"status":"ok","applied_minutes":1,"today_limit":121,"dry_run":false,"mode":"grant"}
```

7. 检查 `sysmodule.log`，预期包含：

```text
grant: applied successfully
```

8. 检查备份文件已生成：

```text
sdmc:/switch/pctltcp-sysmodule/last_pctl_backup.dat
```

9. 回到系统家长控制界面确认当天限制已经增加 1 分钟。
10. 再次输入同一个码，预期：

```json
{"status":"error","reason":"used_token","mode":"grant"}
```

通过标准：

- 第一次有效码真实加时。
- `last_pctl_backup.dat` 写入旧值和新值。
- 同一个码第二次被 `used_token` 拒绝。

## 阶段 5：grant 模式拒绝用例

目的：确认错误码不会修改 PCTL，也不会污染成功路径。

每个用例执行前记录当前系统家长控制当天限制。输入后如果返回错误，限制应保持不变。

| 用例 | 输入 | 预期 reason |
| --- | --- | --- |
| 重复码 | 阶段 4 已成功使用过的同一个码 | `used_token` |
| 错日期码 | `--date` 为非当天 | `wrong_date` |
| 错密钥码 | `--secret wrong-test-secret` | `bad_signature` |
| 超过上限码 | `--minutes 11`，且 `max_add_minutes` 为 10 | `minutes_exceed_limit` |
| 无限状态保护 | 当天无限制，且 `allow_unlimited_to_limited:false` | `unlimited_not_allowed` |

检查点：

```text
sdmc:/switch/pctltcp-sysmodule/grant_result.json
sdmc:/switch/pctltcp-sysmodule/sysmodule.log
sdmc:/switch/pctltcp-sysmodule/used_grants.dat
```

错误码预期不会增加当天限制。`bad_signature`、`wrong_date`、`minutes_exceed_limit` 等验证失败的码也不应被当作成功使用记录。

## 阶段 6：enforce 模式强控制

目的：验证强控制模式下开机会尝试确保 play timer 启用，并在授权写入后刷新 play timer。

只建议在 `grant` 模式稳定后执行。

1. 打包：

```bash
./tools/package_sdmc.sh --strong-control --max-add-minutes 10 --no-allow-unlimited-to-limited
```

等价于包含 `boot2.flag` 且设置 `control_mode=enforce`。

2. 解压到 SD 卡根目录，重启进入 CFW。
3. 检查 `sysmodule.log`。如果 PCTL 可用，预期看到以下之一：

```text
PCTL play timer already enabled.
```

或：

```text
pctl_start_play_timer: OK
```

4. 打开 NRO，输入新的当天有效 1 分钟码。
5. 检查 `grant_result.json`，预期类似：

```json
{"status":"ok","applied_minutes":1,"today_limit":122,"dry_run":false,"mode":"enforce"}
```

6. 检查系统家长控制当天限制确实增加。

通过标准：

- 开机日志显示 play timer 已启用或启用调用成功。
- 有效码写入后返回 `dry_run:false` 和 `mode:"enforce"`。
- 重复码仍被 `used_token` 拒绝。

## 阶段 7：本地时间管理家长区

目的：验证 companion 的隐藏家长区、本地规则文件、今日操作、周模板、bedtime 和 parent unlock。

1. 建议先使用 `observe` 模式：

```bash
./tools/package_sdmc.sh --boot2 --control-mode observe --max-add-minutes 10 --no-allow-unlimited-to-limited
```

2. 打开 `pctltcp-grant.nro`，长按 `L + R + X` 约 2 秒。
3. 输入 `settings.conf` 中的本地设置密码，默认是 `1234`。
4. 依次测试家长区操作：

| 操作 | 按键 | observe 预期 |
| --- | --- | --- |
| 今日 +15/+30/+60 | `A` / `Up` / `Left` | 返回 `dry_run:true`，系统限制不变 |
| 今日固定额度 | `X` 后输入分钟 | 写入 `time_rules.json` 今日 override |
| 今日不限 | `Y` | 写入今日 unlimited override |
| 恢复周模板 | `L` | 清除今日 override 并按周模板计算 |
| 平日/周末模板 | `ZL` | 更新 `time_rules.json` 七天模板 |
| bedtime | `ZR` | 更新 bedtime 开始和恢复分钟 |
| parent unlock | `Minus` | 写入 `time_state.json`，并尝试暂停/恢复 play timer |
| 文件/事件/报告 | `B` | 可查看 `events.jsonl`、`monthly_report.txt` 等 |

5. 检查文件：

```text
sdmc:/switch/pctltcp-sysmodule/time_rules.json
sdmc:/switch/pctltcp-sysmodule/time_state.json
sdmc:/switch/pctltcp-sysmodule/events.jsonl
sdmc:/switch/pctltcp-sysmodule/monthly_report.txt
```

通过标准：

- 孩子主屏不直接暴露家长设置入口。
- 家长区密码保护生效。
- `observe` 模式下家长区操作不修改系统 PCTL 设置。
- `events.jsonl` 记录每次授权、家长操作、拒绝和错误。
- `monthly_report.txt` 明确说明它是本地事件估算，不是官方 per-game 月报。

## 阶段 8：raw 0 今日禁玩验证

目的：验证 raw `0` blocked 行为，并确认只有验证后才允许普通“今日禁止游玩”。

1. 确认 `time_rules.json` 中：

```json
{
  "raw_block_verified": false
}
```

2. 在家长区按 `R` 尝试“今日禁止游玩”。
3. 预期返回：

```json
{"status":"error","reason":"raw_block_not_verified"}
```

4. 切到 `grant` 或 `enforce` 后，在家长区按 `Right` 触发 raw block probe。
5. 检查：

```text
sdmc:/switch/pctltcp-sysmodule/last_pctl_backup.dat
sdmc:/switch/pctltcp-sysmodule/events.jsonl
sdmc:/switch/pctltcp-sysmodule/grant_result.json
```

6. 在真实游戏/软件中确认系统行为：是否立即受限、是否显示提醒、是否能恢复。
7. 确认无误后，再按 `Right` 标记 `raw_block_verified=true`。
8. 再按 `R` 测试普通“今日禁止游玩”。

通过标准：

- 未验证前普通 block 请求被拒绝，不写 PCTL。
- probe 会先写 `last_pctl_backup.dat`。
- 验证后普通 block 请求才允许写 raw `0`。
- 可通过恢复周模板或备份路径恢复当天设置。

## 建议记录表

| 阶段 | control_mode | 输入码 | 预期结果 | 实际结果 | PCTL 是否变化 | 备注 |
| --- | --- | --- | --- | --- | --- | --- |
| NRO-only | observe | 任意码 | 写出 request，可超时 |  | 否 | 无 boot2 |
| disabled | disabled | 有效码 | `reason:"disabled"` |  | 否 | 启动冒烟 |
| observe | observe | 有效码 | `dry_run:true` |  | 否 | 可重复输入 |
| grant | grant | 有效码 | `dry_run:false` |  | 是 | 生成 backup |
| grant | grant | 重复码 | `used_token` |  | 否 |  |
| grant | grant | 错日期码 | `wrong_date` |  | 否 |  |
| grant | grant | 错密钥码 | `bad_signature` |  | 否 |  |
| grant | grant | 超上限码 | `minutes_exceed_limit` |  | 否 |  |
| enforce | enforce | 有效码 | `dry_run:false` |  | 是 | play timer 强控制 |
| observe | observe | 家长区今日操作 | `dry_run:true` |  | 否 | 本地规则更新 |
| grant/enforce | grant/enforce | raw block 未验证 | `raw_block_not_verified` |  | 否 | 保护路径 |
| grant/enforce | grant/enforce | raw block probe | `dry_run:false` |  | 是 | 必须人工确认 |

## 完成标准

真机测试可以认为通过，当满足以下条件：

- `disabled` 模式能启动并拒绝请求，不触碰 PCTL。
- `observe` 模式能验证有效码并返回 `dry_run:true`，系统家长控制不变化。
- `grant` 模式能对有效码真实加时，写入 `last_pctl_backup.dat`，并拒绝重复码。
- 错日期、错密钥、超上限和无限状态保护用例均返回对应 reason，且不修改 PCTL。
- `enforce` 模式在 `grant` 稳定后能正常启动，日志显示 play timer 启用路径，并能完成一次真实加时。
- 家长区隐藏入口、密码保护、本地规则文件、事件日志和月度报告可正常工作。
- raw `0` 禁玩在未验证前被拒绝，probe 验证后才允许作为普通今日禁玩使用。
