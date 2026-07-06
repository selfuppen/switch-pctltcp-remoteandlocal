# switch-pctltcp-offline-grant

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
    "max_add_minutes": 120
}
```

- `device_id`：设备标识，生成离线码时必须一致。
- `grant_secret`：离线码签名密钥，应使用足够长的随机字符串。
- `max_add_minutes`：单个离线码允许增加的最大分钟数。

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
- `X`：进入设置，输入当前密码后修改密码。
- `Y`：刷新上一次 sysmodule 执行结果。
- `Plus`：退出。

输入离线码不需要密码。设置页默认密码是 `1234`。

## 安全提示

- 不要提交真实 `grant_secret`。
- 离线码只对同一天、同一 `device_id` 和同一 `grant_secret` 有效。
- 密码只用于保护 companion 设置页，不是高强度安全边界；它以明文保存在 SD 卡上。
