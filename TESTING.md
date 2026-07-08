# 测试方案

本文档用于验证离线码架构的不同层级。模拟器只能覆盖前端 NRO 和 SD 卡文件交互；真正的 PCTL/sysmodule 端到端行为需要真实 Switch + Atmosphere。

## 测试范围

可在模拟器测试：

- `companion/pctltcp-grant.nro` 能否启动。
- 前端菜单、软键盘输入和按键流程。
- 设置页默认密码 `1234` 是否生效。
- 修改密码后是否写入 `settings.conf`。
- 输入离线码后是否写出 `grant_request.json`。
- 手动创建 `grant_result.json` 后，NRO 是否能读取并显示结果。
- 长按 `L + R + X` 是否能进入密码保护的家长区。
- 家长区操作是否能写出对应 `grant_request.json`。

需要真实 Switch 测试：

- boot2 sysmodule 是否自动启动。
- `sysmodule.log` 是否记录初始化、PCTL play timer 启用和授权处理结果。
- `pctl_init()`、`pctl_start_play_timer()`、`pctl_set_day_limit_minutes()` 是否成功。
- 有效离线码是否真实增加当天可玩时间。
- 重复码、错日期码、错密钥码、超过 `max_add_minutes` 的码是否被拒绝。

## 通用准备

构建产物：

```text
pctltcp-sysmodule.nsp
companion/pctltcp-grant.nro
```

前端 NRO：

```text
companion/pctltcp-grant.nro
```

Switch 端配置目录：

```text
sdmc:/switch/pctltcp-sysmodule/
```

需要的配置文件：

```text
grant.conf
settings.conf
time_rules.json
```

示例 `grant.conf`：

```json
{
  "device_id": "kid-switch",
  "grant_secret": "change-me-to-a-long-random-secret",
  "max_add_minutes": 120
}
```

示例 `settings.conf`：

```text
1234
```

示例 `time_rules.json` 可使用仓库根目录的 `time_rules.json.example`。

固定测试码生成命令：

```bash
python tools/grant_code.py \
  --minutes 30 \
  --device kid-switch \
  --secret change-me-to-a-long-random-secret \
  --date 2026-07-06 \
  --nonce 0x1234
```

预期输出：

```text
20F0-SSGJ-6JEA-AEJE
```

## Ryujinx 模拟器

### 目录准备

Ryujinx 的 SD 卡目录常见位置：

```text
%APPDATA%\Ryujinx\sdcard
```

展开后通常是：

```text
C:\Users\<用户名>\AppData\Roaming\Ryujinx\sdcard
```

便携版可检查：

```text
<Ryujinx目录>\portable\sdcard
```

在 SD 卡目录下创建：

```text
sdcard\switch\pctltcp-sysmodule\
```

放入：

```text
sdcard\switch\pctltcp-sysmodule\grant.conf
sdcard\switch\pctltcp-sysmodule\settings.conf
```

### 启动前端

在 Ryujinx 中通过加载文件的方式打开：

```text
companion\pctltcp-grant.nro
```

只运行 NRO 时，不会自动运行 `pctltcp-sysmodule.nsp`。

### 用例 1：前端启动

步骤：

1. 打开 `pctltcp-grant.nro`。
2. 确认屏幕显示 `Pctl Offline Grant`。
3. 确认菜单包含输入离线码、最近结果、刷新时间和退出提示。

预期：

- NRO 不崩溃。
- 菜单内容和按键提示正确。

### 用例 2：默认密码进入家长区并修改密码

步骤：

1. 确认 `settings.conf` 内容为 `1234`，或删除 `settings.conf` 让前端走默认密码。
2. 启动 NRO。
3. 长按 `L + R + X` 约 2 秒。
4. 输入 `1234`。
5. 在家长区按 `Down` 进入修改密码。
6. 输入新密码，例如 `5678`。
7. 退出 NRO。
8. 检查：

```text
%APPDATA%\Ryujinx\sdcard\switch\pctltcp-sysmodule\settings.conf
```

预期：

- 文件内容变为：

```text
5678
```

- 再次进入设置时，`1234` 应失败，`5678` 应成功。

### 用例 3：输入离线码写请求文件

步骤：

1. 启动 NRO。
2. 按 `A`。
3. 输入：

```text
20F0-SSGJ-6JEA-AEJE
```

4. NRO 会等待 sysmodule 返回结果；模拟器中一般会超时。
5. 检查：

```text
%APPDATA%\Ryujinx\sdcard\switch\pctltcp-sysmodule\grant_request.json
```

预期内容类似：

```json
{"type":"offline_code","code":"20F0-SSGJ-6JEA-AEJE"}
```

说明：

- 如果文件存在且内容正确，说明前端输入到 SD 卡请求文件的链路正常。
- 超时是预期行为，因为模拟器没有运行后台 sysmodule。

### 用例 4：家长区请求文件

步骤：

1. 启动 NRO。
2. 长按 `L + R + X` 约 2 秒并输入设置密码。
3. 在家长区按 `X`，输入 `90` 作为今日固定额度。
4. 因为模拟器没有后台 sysmodule，等待结果可能超时。
5. 检查：

```text
%APPDATA%\Ryujinx\sdcard\switch\pctltcp-sysmodule\grant_request.json
```

预期内容类似：

```json
{"type":"set_today_limit","minutes":90}
```

### 用例 5：手动模拟 sysmodule 结果

步骤：

1. 手动创建：

```text
%APPDATA%\Ryujinx\sdcard\switch\pctltcp-sysmodule\grant_result.json
```

2. 写入：

```json
{"status":"ok","applied_minutes":30,"today_limit":120}
```

3. 启动或回到 NRO。
4. 按 `B` 读取最近结果，或按 `Y` 刷新时间状态。

预期：

- `Last result` 显示上述 JSON。

### 日志检查

Ryujinx 日志常见位置：

```text
%APPDATA%\Ryujinx\Logs
```

或：

```text
<Ryujinx程序目录>\Logs
```

建议搜索关键词：

```text
pctltcp
pctltcp-grant
settings.conf
grant_request
grant_result
sdmc
pctl
stub
error
```

项目自己的 sysmodule 日志路径为：

```text
%APPDATA%\Ryujinx\sdcard\switch\pctltcp-sysmodule\sysmodule.log
```

模拟器里通常不会生成该文件；生成与否不能作为前端测试失败依据。

## Eden 模拟器

### 目录准备

优先用 Eden 菜单打开用户目录：

```text
File -> Open Eden Folder
```

Eden 的 SD 卡目录常见位置：

```text
%APPDATA%\Eden\sdmc
```

展开后通常是：

```text
C:\Users\<用户名>\AppData\Roaming\Eden\sdmc
```

如果是便携版，在 Eden 用户目录下寻找：

```text
sdmc
```

在 SD 卡目录下创建：

```text
sdmc\switch\pctltcp-sysmodule\
```

放入：

```text
sdmc\switch\pctltcp-sysmodule\grant.conf
sdmc\switch\pctltcp-sysmodule\settings.conf
```

### 启动前端

在 Eden 中加载：

```text
companion\pctltcp-grant.nro
```

只运行 NRO 时，不会自动运行 `pctltcp-sysmodule.nsp`。

### 用例 1：前端启动

步骤：

1. 打开 `pctltcp-grant.nro`。
2. 确认屏幕显示 `Pctl Offline Grant`。
3. 确认菜单项显示正确。

预期：

- NRO 正常进入主界面。
- 没有启动崩溃或明显输入异常。

### 用例 2：家长区和设置密码读写

步骤：

1. 确认：

```text
%APPDATA%\Eden\sdmc\switch\pctltcp-sysmodule\settings.conf
```

内容为：

```text
1234
```

2. 启动 NRO。
3. 长按 `L + R + X` 约 2 秒。
4. 输入 `1234`。
5. 在家长区按 `Down`。
6. 输入新密码，例如 `5678`。
7. 退出 NRO。
8. 重新打开 `settings.conf`。

预期：

- `settings.conf` 内容变为 `5678`。
- 旧密码失败，新密码成功。

### 用例 3：输入离线码写请求文件

步骤：

1. 启动 NRO。
2. 按 `A`。
3. 输入：

```text
20F0-SSGJ-6JEA-AEJE
```

4. 等待超时或退出。
5. 检查：

```text
%APPDATA%\Eden\sdmc\switch\pctltcp-sysmodule\grant_request.json
```

预期内容类似：

```json
{"type":"offline_code","code":"20F0-SSGJ-6JEA-AEJE"}
```

说明：

- 文件出现且内容正确即通过。
- 等待结果超时是预期行为。

### 用例 4：家长区请求文件

步骤：

1. 启动 NRO。
2. 长按 `L + R + X` 约 2 秒并输入设置密码。
3. 在家长区按 `A` 测试今日 +15 分钟。
4. 等待超时或退出。
5. 检查：

```text
%APPDATA%\Eden\sdmc\switch\pctltcp-sysmodule\grant_request.json
```

预期内容类似：

```json
{"type":"add_today_minutes","minutes":15}
```

### 用例 5：手动模拟 sysmodule 结果

步骤：

1. 手动创建：

```text
%APPDATA%\Eden\sdmc\switch\pctltcp-sysmodule\grant_result.json
```

2. 写入：

```json
{"status":"error","reason":"used_token"}
```

3. 启动或回到 NRO。
4. 按 `B` 读取最近结果，或按 `Y` 刷新时间状态。

预期：

- `Last result` 显示该错误 JSON。

### 日志检查

优先用 Eden 菜单打开日志目录：

```text
File -> Open Eden Folder -> Log Folder
```

Windows 常见位置：

```text
%APPDATA%\Eden\log
```

建议搜索关键词：

```text
pctltcp
pctltcp-grant
settings.conf
grant_request
grant_result
sdmc
pctl
stub
error
```

项目自己的 sysmodule 日志路径为：

```text
%APPDATA%\Eden\sdmc\switch\pctltcp-sysmodule\sysmodule.log
```

模拟器里通常不会生成该文件；生成与否不能作为前端测试失败依据。

## 真实 Switch + Atmosphere 端到端测试

详细的 `grant.conf` `control_mode` 说明和低风险到强控制的分阶段真机流程见 `REAL_SWITCH_TESTING.md`。本节保留端到端用例摘要。

### 安装

构建后打包：

```bash
make
./tools/package_sdmc.sh
```

将 zip 解压到 SD 卡根目录。确认路径：

```text
sdmc:/atmosphere/contents/010000000000BD23/exefs.nsp
sdmc:/atmosphere/contents/010000000000BD23/toolbox.json
sdmc:/atmosphere/contents/010000000000BD23/flags/boot2.flag
sdmc:/switch/pctltcp-grant.nro
sdmc:/switch/pctltcp-sysmodule/grant.conf
sdmc:/switch/pctltcp-sysmodule/settings.conf
sdmc:/switch/pctltcp-sysmodule/time_rules.json
```

重启进入 CFW。

### 用例 1：sysmodule 启动日志

检查：

```text
sdmc:/switch/pctltcp-sysmodule/sysmodule.log
```

预期包含：

```text
pctltcp-sysmodule starting (offline grant mode)
grant: config loaded
pctltcp-sysmodule initialization complete (offline grant mode)
```

若 PCTL 可用，还应看到 play timer 已启用或启动结果。

### 用例 2：有效码加时

步骤：

1. 生成当天有效离线码。
2. 打开 `pctltcp-grant.nro`。
3. 按 `A` 输入离线码。
4. 等待结果。

预期：

- `grant_result.json` 类似：

```json
{"status":"ok","applied_minutes":30,"today_limit":120}
```

- `sysmodule.log` 包含：

```text
grant: applied successfully
```

### 用例 3：重复码拒绝

步骤：

1. 再次输入同一个离线码。

预期：

```json
{"status":"error","reason":"used_token"}
```

### 用例 4：错日期码拒绝

步骤：

1. 用非当天日期生成离线码。
2. 输入该码。

预期：

```json
{"status":"error","reason":"wrong_date"}
```

### 用例 5：错密钥码拒绝

步骤：

1. 用错误 `--secret` 生成离线码。
2. 输入该码。

预期：

```json
{"status":"error","reason":"bad_signature"}
```

### 用例 6：超过分钟上限拒绝

步骤：

1. 确认 `grant.conf` 中 `max_add_minutes` 为 `120`。
2. 生成 `--minutes 121` 的离线码。
3. 输入该码。

预期：

```json
{"status":"error","reason":"minutes_exceed_limit"}
```

## 结论判定

模拟器通过条件：

- NRO 能启动。
- 设置密码能读写 `settings.conf`。
- 输入离线码能写出 `grant_request.json`。
- 手动创建 `grant_result.json` 后，按 `Y` 能显示结果。
- 家长区能写出今日管理、周模板、bedtime 或 parent unlock 请求。

真机通过条件：

- sysmodule 开机自动运行并写日志。
- 有效码真实加时。
- 无效码按原因拒绝。
- `used_grants.dat` 能阻止同一日期和 nonce 重复使用。
- 家长区本地规则、事件日志、月度报告和 raw `0` 验证流程符合 `REAL_SWITCH_TESTING.md`。
