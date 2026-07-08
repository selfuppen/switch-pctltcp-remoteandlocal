# 仓库协作指南

## 项目结构与模块组织

本仓库包含一个 Nintendo Switch 家长控制离线授权 sysmodule、一个前台授权 NRO，以及配套的家长侧离线码生成工具。

- `source/`：Switch sysmodule 的 C 实现。`main.c` 负责启动、运行时初始化和离线请求轮询，`pctl_handler.*` 封装家长控制操作，`grant_manager.*` 处理离线授权码验证、加时和防重复使用记录。
- `companion/`：前台 NRO companion，用于让孩子输入离线授权码，并提供受密码保护的设置页。
- `tools/`：家长侧工具和打包脚本，例如 `grant_code.py` 与 `package_sdmc.sh`。
- `pctltcp-sysmodule.json` 和 `toolbox.json`：Switch 打包与 boot2 启动元数据。
- `grant.conf.example`：Switch 端离线授权配置示例。
- `settings.conf.example`：companion 设置密码示例。
- `README.md`：仓库入口说明。
- `docs/`：面向用户、测试、架构和产品设计的说明性文档，例如 `docs/GRANTS.md`、`docs/TESTING.md` 和 `docs/REAL_SWITCH_TESTING.md`。

目前没有专门的 `tests/` 或资产目录。

## 构建、测试与开发命令

- `make`：使用 devkitPro/libnx 构建 `pctltcp-sysmodule.nsp` 和 `companion/pctltcp-grant.nro`。需要设置 `DEVKITPRO`。
- `make clean`：删除生成的构建产物，包括 `build/`、`.elf`、`.nso`、`.npdm`、`.nsp`，以及 companion 的构建产物。
- `./tools/package_sdmc.sh`：将 NSP、NRO、boot2 标记、授权配置和设置密码示例打包为可直接解压到 SD 卡根目录的 zip。
- `python tools/grant_code.py --minutes 30 --device kid-switch --secret <secret>`：生成当天有效的离线加时授权码。
- `python tools/grant_code.py --minutes 30 --device kid-switch --secret <secret> --date 2026-07-06 --nonce 0x1234`：生成固定日期/nonce 的可复现测试码。

## 代码风格与命名约定

C 和 Python 均使用 4 空格缩进。C 函数和变量使用 `snake_case`，常量和宏使用 `UPPER_SNAKE_CASE`，头文件与实现文件成对维护。函数左大括号保持当前项目风格：放在函数声明同一行。Python 保持直接清晰的 CLI 写法。修改 sysmodule 启动、PCTL、授权或设置密码路径时避免大范围重构。

## 测试指南

目前没有自动化测试套件。Switch 端修改至少运行 `make`，确认生成 NSP 和 NRO。离线授权修改应使用 `tools/grant_code.py` 生成固定日期/nonce 的测试码，并在 Switch 或模拟器上验证有效码、重复码、错日期码、错密钥码和超过 `max_add_minutes` 的码。companion 修改应验证输入离线码无需密码，设置页默认 `1234` 可进入，修改密码后旧密码失败、新密码成功。

## 提交与 Pull Request 指南

近期历史使用简洁前缀，例如 `feat:`、`fix:`、`docs:`，也有版本型提交如 `v2.0.0:`。优先使用祈使语气和清晰范围，例如 `feat(grants): add offline play-time grants`。Pull Request 应描述行为变化，列出 Switch/companion 验证内容，并说明配置变化。

## 安全与配置提示

不要提交真实 `grant_secret` 或真实家庭配置。保持 `grant.conf.example` 和 `settings.conf.example` 通用化。`grant_secret` 是离线授权签名密钥，应使用足够长的随机字符串，并避免提交真实值。companion 设置密码只是本地 UI 保护，明文保存在 SD 卡上，不应视为强安全边界。
