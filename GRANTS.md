# 离线授权码

离线授权码允许家长在没有网络和服务端的情况下，为当天增加 Switch 家长控制可玩时间。

## Switch 端配置

配置文件路径：

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

- `device_id` 必须和生成码时的 `--device` 一致。
- `grant_secret` 必须和生成码时的 `--secret` 一致。
- `max_add_minutes` 限制单个离线码最多能增加多少分钟。

## 生成授权码

```bash
python tools/grant_code.py \
  --minutes 30 \
  --device kid-switch \
  --secret change-me-to-a-long-random-secret
```

第一行输出就是可以告诉孩子输入的短码，例如：

```text
28G1-PRF6-FTNN-PD9X
```

用于测试时可以固定日期和 nonce：

```bash
python tools/grant_code.py \
  --minutes 30 \
  --device kid-switch \
  --secret change-me-to-a-long-random-secret \
  --date 2026-07-06 \
  --nonce 0x1234
```

## Companion NRO

构建产物：

```text
companion/pctltcp-grant.nro
```

菜单：

- `A`：输入离线授权码。
- `B`：输入设置密码后查看 `grant_result.json`、`grant_request.json`、`sysmodule.log`、`grant.conf`、`settings.conf` 等文件。
- `X`：进入设置，输入当前密码后修改密码。
- `Y`：重新读取上一次 sysmodule 执行结果。
- `Plus`：退出。

输入离线码不需要密码。设置页密码保存在：

```text
sdmc:/switch/pctltcp-sysmodule/settings.conf
```

缺失或为空时默认密码为 `1234`。

NRO 会写入 `grant_request.json`，sysmodule 会写入 `grant_result.json`。
文件预览会显示配置内容，其中 `grant.conf` 可能包含 `grant_secret`，`settings.conf` 可能包含明文设置密码。

## 打包到 SD 卡

在 devkitPro 构建环境中完成构建后：

```bash
make
./tools/package_sdmc.sh
```

脚本会生成 `pctltcp-offline-grant-sdmc.zip`，其中包含：

```text
atmosphere/contents/010000000000BD23/exefs.nsp
atmosphere/contents/010000000000BD23/toolbox.json
atmosphere/contents/010000000000BD23/flags/boot2.flag
switch/pctltcp-grant.nro
switch/pctltcp-sysmodule/grant.conf
switch/pctltcp-sysmodule/settings.conf
```

如果仓库根目录存在真实 `grant.conf` 或 `settings.conf`，脚本会优先打包真实配置；否则使用示例文件。

## Token 格式

授权码是 16 个 Crockford Base32 字符，解码后共 10 字节：

- 48-bit payload：版本、动作、分钟数、日期、nonce。
- 32-bit signature：HMAC-SHA256 截断到 4 字节。

HMAC 输入：

```text
device_id || NUL || payload_bytes
```

密钥使用 `grant_secret` 中的 UTF-8 文本。

每个有效授权码在同一台 Switch 上只能使用一次，因为 sysmodule 会把日期和 nonce 记录到 `used_grants.dat`。
