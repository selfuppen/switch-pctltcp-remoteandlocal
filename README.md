# switch-pctltcp-offline-grant

真机测试请先阅读 `SAFE_TESTING.md`。默认配置使用 `control_mode=observe`，打包脚本默认不放 `boot2.flag`，用于避免一上机就进入强控制。

Nintendo Switch 家长控制离线加时工具。项目由三部分组成：

- boot2 sysmodule：开机自动运行，验证离线授权码并调用 PCTL 增加当天可玩时间。
- 前台 companion NRO：让孩子输入离线授权码；设置页用本地密码保护。
- 家长侧工具：生成当天有效、一次性使用的短授权码。

## 功能

- 开机自动启动 sysmodule，并尝试确保 PCTL play timer 已开启。
- 输入离线码即可增加当天限额，不需要网络和服务端。
- 每个离线码带日期和 nonce，同一台 Switch 上只能使用一次。
- 单个离线码可增加的分钟数受 `max_add_minutes` 限制。
- companion 设置页默认密码为 `1234`，可在 Switch 上修改。

## 文件结构

```text
source/                 sysmodule C 代码
companion/              前台 NRO companion
tools/grant_code.py     家长侧离线码生成工具
tools/package_sdmc.sh   SD 卡安装包打包脚本
grant.conf.example      Switch 端离线码配置示例
settings.conf.example   companion 设置密码示例
GRANTS.md               离线码格式和使用说明
TESTING.md              真机与模拟器测试方案
REAL_SWITCH_TESTING.md  真机 control_mode 分阶段测试指南
```

## 构建

需要 devkitPro + libnx，并设置 `DEVKITPRO`。

```bash
make
```

产物：

```text
pctltcp-sysmodule.nsp
companion/pctltcp-grant.nro
```

清理：

```bash
make clean
```

## 安装到 SD 卡

构建完成后生成安装 zip：

```bash
./tools/package_sdmc.sh
```

脚本会生成带秒级时间戳的安装包，例如 `pctltcp-offline-grant-sdmc-2026-07-06_15-04-05.zip`，解压到 SD 卡根目录即可。主要路径如下：

```text
sdmc:/atmosphere/contents/010000000000BD23/exefs.nsp
sdmc:/atmosphere/contents/010000000000BD23/toolbox.json
sdmc:/atmosphere/contents/010000000000BD23/flags/boot2.flag
sdmc:/switch/pctltcp-grant.nro
sdmc:/switch/pctltcp-sysmodule/grant.conf
sdmc:/switch/pctltcp-sysmodule/settings.conf
```

安装后重启 Switch 或重新进入 CFW。

## 配置

编辑 SD 卡上的：

```text
sdmc:/switch/pctltcp-sysmodule/grant.conf
```

示例：

```json
{
    "device_id": "kid-switch",
    "grant_secret": "change-me-to-a-long-random-secret",
    "max_add_minutes": 120,
    "control_mode": "observe",
    "allow_unlimited_to_limited": false
}
```

- `device_id`：设备标识，生成离线码时必须一致。
- `grant_secret`：离线码签名密钥，应使用足够长的随机字符串。
- `max_add_minutes`：单个离线码允许增加的最大分钟数。
- `control_mode`：默认 `observe`，只验证和读取，不写 PCTL；稳定后可切换到 `grant` 或 `enforce`。
- `allow_unlimited_to_limited`：默认 `false`，避免把无限制/未配置的当天改成有限制；如果设为 `true`，无限制当天会按 0 分钟起算再叠加授权分钟数。

companion 设置密码保存在：

```text
sdmc:/switch/pctltcp-sysmodule/settings.conf
```

缺失或为空时默认密码为 `1234`。

## 生成离线码

```bash
python tools/grant_code.py \
  --minutes 30 \
  --device kid-switch \
  --secret change-me-to-a-long-random-secret
```

输出示例：

```text
28G1-PRF6-FTNN-PD9X
```

可用固定日期和 nonce 生成可复现测试码：

```bash
python tools/grant_code.py \
  --minutes 30 \
  --device kid-switch \
  --secret change-me-to-a-long-random-secret \
  --date 2026-07-06 \
  --nonce 0x1234
```

## Switch 上使用

打开 `pctltcp-grant.nro`：

- `A`：输入离线授权码。
- `B`：重新读取最近一次 sysmodule 执行结果。
- `Y`：刷新当前时间状态。
- 长按 `L + R + X`：输入设置密码后进入家长区。
- `Plus`：退出。

输入离线码不需要密码。设置页默认密码是 `1234`。
文件预览会显示 `grant.conf` 和 `settings.conf`，其中可能包含授权密钥或明文设置密码，请只在可信环境下查看。

## 本地时间管理

新版本会在 `sdmc:/switch/pctltcp-sysmodule/` 下维护：

```text
time_rules.json
time_state.json
events.jsonl
monthly_report.txt
```

`time_rules.json` 保存一周模板、今日临时规则、bedtime、提醒/强制模式，以及 raw `0` 禁玩和 `Suspend Software` 的真机验证开关。默认可参考仓库中的 `time_rules.json.example`。

孩子主屏只显示今日额度、剩余时间、当前状态和离线加时码入口。家长区通过主屏长按 `L + R + X` 约 2 秒呼出，并需要输入本地设置密码。家长区支持今日固定额度、今日加时、今日不限、今日禁玩、恢复周模板、平日/周末模板、bedtime、临时 parent unlock、文件/事件记录查看和修改本地密码。

`raw 0` 禁玩默认未启用。需要先在家长区触发 raw block probe，在真机确认行为后再标记 `raw_block_verified=true`，之后才允许使用“今日禁止游玩”。

## 安全提示

- 不要提交真实 `grant_secret`。
- 离线码只对同一天、同一 `device_id` 和同一 `grant_secret` 有效。
- 如需临时停用 sysmodule，可在 SD 卡创建 `sdmc:/switch/pctltcp-sysmodule/disable.flag` 进入 fail-open 状态，即停止处理授权并避免触碰 PCTL。
- 密码只用于保护 companion 设置页，不是高强度安全边界；它以明文保存在 SD 卡上。
