# Nintendo Switch 家长控制离线加时重构技术架构设计

本文档面向全新项目重构，目标是在保留现有产品能力的前提下，设计一个更清晰、更可测试、更安全的技术架构。它不要求逐字兼容当前实现，但建议保留关键外部契约，例如 SD 卡工作目录、离线授权码语义、控制模式和 fail-open 行为。

## 1. 架构目标

### 1.1 必须实现的能力

- 在无网络、无服务端场景下，通过短授权码为当天增加 Switch 家长控制可玩时间。
- 提供 Switch 前台 companion NRO，让孩子输入授权码，让家长进入密码保护的本地管理区。
- 提供 Switch 后台 sysmodule，负责验证请求、读取/写入 PCTL、维护本地规则和事件。
- 提供家长侧 CLI 工具，生成当天有效、一次性使用的授权码。
- 支持本地时间规则：周模板、今日临时规则、今日不限、今日禁玩、bedtime、parent unlock。
- 支持低风险到强控制的分阶段运行模式：`disabled`、`observe`、`grant`、`enforce`。
- 支持可恢复机制：`disable.flag` fail-open、boot2 flag 停用、写入前 PCTL 备份。

### 1.2 重构质量目标

- 模块边界清晰：授权码、文件协议、PCTL、时间规则、UI、日志分别隔离。
- 核心逻辑可单元测试：授权码验证、规则计算、请求处理不依赖 Switch IPC。
- 写入路径可审计：所有改变系统状态的操作都有备份、事件记录和明确错误码。
- 默认安全：默认 observe，不主动写 PCTL，不把无限制误改为有限制。
- 数据协议可演进：请求、结果、规则文件都带版本字段。
- 真机风险可控：高风险能力必须显式验证后启用。

## 2. 总体架构

```text
┌──────────────────────────┐
│ Parent CLI / Desktop Tool │
│ 生成离线授权码             │
└─────────────┬────────────┘
              │ 短码
              ▼
┌──────────────────────────┐
│ Companion NRO             │
│ 孩子输入 / 家长设置 UI      │
└─────────────┬────────────┘
              │ grant_request.json
              ▼
┌──────────────────────────┐
│ Sysmodule Request Engine  │
│ 轮询、分发、写结果          │
└─────────────┬────────────┘
              │
      ┌───────┼────────────────────────────────┐
      ▼       ▼                                ▼
┌──────────┐ ┌──────────────┐          ┌─────────────┐
│ Auth Core│ │ Time Rule Core│          │ PCTL Adapter │
│ 验码防重 │ │ 今日规则计算   │          │ Switch IPC    │
└────┬─────┘ └──────┬───────┘          └──────┬──────┘
     │              │                         │
     └───────┬──────┴───────────────┬─────────┘
             ▼                      ▼
      ┌──────────────┐       ┌───────────────┐
      │ Storage Layer │       │ Audit / Report│
      │ SD 卡原子读写 │       │ 日志事件月报   │
      └──────────────┘       └───────────────┘
```

## 3. 项目拆分建议

建议将新项目拆成以下目录：

```text
source/
  app/
    main.c
    service_lifecycle.c
  core/
    auth_token.c
    auth_token.h
    request_dispatcher.c
    request_dispatcher.h
    time_rules.c
    time_rules.h
    control_policy.c
    control_policy.h
  platform/
    pctl_adapter.c
    pctl_adapter.h
    switch_time.c
    switch_time.h
    switch_fs.c
    switch_fs.h
  storage/
    json_codec.c
    json_codec.h
    atomic_file.c
    atomic_file.h
    config_store.c
    config_store.h
    ledger_store.c
    ledger_store.h
    event_store.c
    event_store.h
  diagnostics/
    logger.c
    logger.h
    report_writer.c
    report_writer.h

companion/
  source/
    main.c
    ui_home.c
    ui_parent.c
    request_client.c
    settings_password.c
    file_preview.c

tools/
  grant_code.py
  protocol_test.py
  package_sdmc.sh

tests/
  unit/
    test_auth_token.c
    test_time_rules.c
    test_request_dispatcher.c
  fixtures/
    grant.conf
    time_rules.json
    requests/
    results/
```

如果当前构建体系不方便直接跑 C 单元测试，也应至少将 `core/` 设计为不依赖 libnx 的纯 C，以便后续用 host-side runner 编译验证。

## 4. 模块设计

### 4.1 Service Lifecycle

职责：

- 处理 sysmodule 启动延迟、服务初始化和退出清理。
- 初始化 SM、FS、Time、SDMC。
- 创建工作目录。
- 初始化日志、配置、规则、状态和请求引擎。
- 根据 `control_mode` 决定是否执行开机 play timer enforcement。
- 主循环中定时轮询请求、刷新 parent unlock、处理 bedtime transition、输出心跳日志。

设计要点：

- boot2 启动时继续保留延迟启动，避免系统服务未就绪。
- 初始化失败不应直接死循环崩溃，应进入降级请求循环并写日志。
- 所有 PCTL 访问必须通过 `PctlAdapter`，生命周期层不直接操作 IPC。

### 4.2 Storage Layer

职责：

- 提供文本、JSON、ledger、事件日志的读写接口。
- 提供原子写：写入 `*.tmp`，flush/close 后 rename。
- 统一管理 SD 卡路径。
- 提供文件存在性检查，例如 `disable.flag`。

建议保留路径：

```text
sdmc:/switch/pctltcp-sysmodule/
```

关键接口：

```c
bool storage_read_text(const char *path, char *buf, size_t size);
bool storage_write_text_atomic(const char *path, const char *text);
bool storage_append_line(const char *path, const char *line);
bool storage_file_exists(const char *path);
bool storage_remove(const char *path);
```

改进建议：

- 所有结果文件和规则文件都使用原子写。
- 读取 JSON 时限制最大文件大小，避免异常大文件耗尽 sysmodule 内存。
- 对配置文件读取失败要返回结构化错误，而不是悄悄使用半初始化状态。

### 4.3 JSON Codec

当前项目使用轻量字符串查找。新架构建议把 JSON 编解码集中到一个模块。

目标：

- 支持严格字段解析。
- 支持默认值和字段范围校验。
- 支持向后兼容旧版本字段。
- 支持稳定输出顺序，方便 diff 和人工检查。

如果引入第三方 JSON parser 成本过高，可实现一个受限 JSON codec，但必须集中维护，不应散落在业务代码里。

所有协议对象建议带 `version`：

```json
{
  "version": 1,
  "type": "offline_code",
  "code": "20F0-SSGJ-6JEA-AEJE"
}
```

### 4.4 Config Store

职责：

- 读取和校验 `grant.conf`。
- 提供完整默认值。
- 解析 `control_mode` 和安全开关。
- 隐藏敏感字段的日志输出。

配置结构建议：

```c
typedef struct {
    char device_id[64];
    char grant_secret[128];
    int max_add_minutes;
    ControlMode control_mode;
    bool allow_unlimited_to_limited;
    bool raw_block_enabled;
    bool suspend_enabled;
    bool loaded;
} AppConfig;
```

默认值：

- `max_add_minutes = 120`
- `control_mode = observe`
- `allow_unlimited_to_limited = false`
- 高风险能力默认关闭

校验规则：

- `device_id` 必须非空。
- `grant_secret` 必须非空，建议长度不少于 32 字节。
- `max_add_minutes` 范围建议为 1 到 1440。
- 未知 `control_mode` 降级为 `observe` 并写警告日志。

### 4.5 Auth Token Core

职责：

- Crockford Base32 编解码。
- token payload 打包/解包。
- HMAC-SHA256 签名校验。
- 日期、版本、动作、分钟数校验。
- nonce 防重放检查。

推荐 token 语义：

```text
payload:
  version
  action
  minutes
  date_key
  nonce

signature:
  trunc32(HMAC-SHA256(secret, device_id || NUL || payload))
```

核心接口：

```c
typedef enum {
    AUTH_OK = 0,
    AUTH_BAD_CODE,
    AUTH_BAD_VERSION,
    AUTH_UNSUPPORTED_ACTION,
    AUTH_BAD_SIGNATURE,
    AUTH_BAD_CLOCK,
    AUTH_WRONG_DATE,
    AUTH_USED_TOKEN,
    AUTH_MINUTES_EXCEED_LIMIT
} AuthResult;

AuthResult auth_verify_code(
    const char *code,
    const AppConfig *cfg,
    uint16_t today_key,
    const UsedGrantLedger *ledger,
    GrantToken *out
);
```

重要原则：

- 验证失败不写 used ledger。
- `observe` 成功也不写 used ledger。
- 只有 `grant`/`enforce` 写 PCTL 成功后才追加 used ledger。
- token 校验结果不直接写 PCTL，只返回可执行动作。

### 4.6 Used Grant Ledger

职责：

- 记录已成功使用的 `(date_key, nonce)`。
- 判断 token 是否重复使用。
- 支持测试时清理。

现有 `used_grants.dat` 可继续使用文本格式：

```text
0C86 1234
0C86 ABCD
```

改进建议：

- 写入前加载去重，避免重复行。
- 文件过大时按日期清理旧记录。
- 追加失败时成功结果应带 warning，并写日志；是否回滚 PCTL 需要单独设计，默认不建议自动回滚。

### 4.7 Time Rule Core

职责：

- 解析 `time_rules.json`。
- 根据当前日期、星期、今日 override、bedtime 和 parent unlock 计算当前目标规则。
- 生成需要应用到 PCTL 的目标状态。
- 不直接读写 PCTL。

核心模型：

```c
typedef enum {
    RULE_LIMIT,
    RULE_UNLIMITED,
    RULE_BLOCKED
} RuleMode;

typedef struct {
    RuleMode mode;
    int minutes;
} DayRule;

typedef struct {
    DayRule week[7];
    bool today_override;
    uint16_t today_date_key;
    DayRule today;
    bool bedtime_enabled;
    int bedtime_start_min;
    int bedtime_end_min;
    LimitAction limit_action;
    bool raw_block_verified;
    bool suspend_verified;
} TimeRules;
```

规则优先级：

1. `parent_unlock_active`：临时暂停 play timer，不主动施加限制。
2. bedtime active：根据 `limit_action` 决定提醒或强制限制。
3. 今日 override：如果日期匹配，则使用今日规则。
4. 周模板：按当前星期使用默认规则。

范围校验：

- 分钟数范围 1 到 1440。
- bedtime 分钟范围 0 到 1439。
- 今日 override 必须绑定 date_key。

### 4.8 Control Policy

职责：

- 将请求、配置、当前模式和规则计算结果转成执行计划。
- 决定是否 dry run。
- 决定是否允许从 unlimited 转为 limited。
- 决定 raw block 和 suspend 是否可执行。

控制模式策略：

```text
disabled:
  拒绝所有会改变状态的请求，不读写 PCTL。

observe:
  允许验证授权码和计算结果。
  可读 PCTL。
  不写 PCTL。
  不消费 nonce。

grant:
  有效授权码和家长区请求可以写今天限制。
  不在开机主动启用 play timer。

enforce:
  grant 的全部能力。
  开机确保 play timer 启用。
  写入后刷新 play timer。
  bedtime/suspend 类强控制只应在此模式生效。
```

执行计划建议：

```c
typedef struct {
    bool allowed;
    bool dry_run;
    bool writes_pctl;
    bool consumes_nonce;
    bool requires_backup;
    PctlTarget target;
    ErrorCode error;
} ControlPlan;
```

所有请求先生成 `ControlPlan`，再由执行层操作 PCTL 和 storage。

### 4.9 PCTL Adapter

职责：

- 封装 libnx PCTL 初始化、退出和 IPC 调用。
- 提供业务友好的状态读取和目标写入接口。
- 隐藏 raw settings 布局。
- 写入前创建备份。

推荐接口：

```c
typedef struct {
    bool has_limited_today;
    bool blocked_today;
    bool unrestricted_today;
    int today_limit;
    bool remaining_available;
    uint32_t remaining_minutes;
    bool play_timer_enabled;
    bool restricted_now;
    int today;
    uint16_t date_key;
} PctlStatus;

Result pctl_adapter_read_status(PctlStatus *out);
Result pctl_adapter_apply_limit(int minutes, PctlBackup *backup);
Result pctl_adapter_apply_unlimited(PctlBackup *backup);
Result pctl_adapter_apply_blocked(PctlBackup *backup);
Result pctl_adapter_start_timer(void);
Result pctl_adapter_stop_timer(void);
```

实现原则：

- PCTL session 生命周期由 adapter 管理。
- 对写入调用加互斥锁。
- 所有 raw offset 常量只存在于 adapter 内部。
- 写入前必须读取当前 settings 并保存备份。
- raw `0` 禁玩必须检查 `raw_block_verified`。
- `Suspend Software` 或类似高风险动作必须检查 `suspend_verified`。

### 4.10 Request Dispatcher

职责：

- 读取 `grant_request.json`。
- 校验请求版本和类型。
- 调用对应 handler。
- 删除请求文件。
- 写 `grant_result.json`。
- 写事件日志和月报。

请求类型建议：

| type | 说明 |
| --- | --- |
| `offline_code` | 输入离线授权码 |
| `status` | 刷新状态 |
| `set_today_limit` | 设置今日固定额度 |
| `add_today_minutes` | 今日加时 |
| `disable_today_limit` | 今日不限 |
| `block_today` | 今日禁玩 |
| `probe_raw_block` | raw block 真机验证 |
| `restore_today_policy` | 恢复周模板 |
| `set_weekly_template` | 设置周模板 |
| `set_bedtime` | 设置 bedtime |
| `set_limit_action` | 设置限制动作和验证开关 |
| `parent_unlock_start` | 开始临时解锁 |
| `parent_unlock_end` | 结束临时解锁 |

结果结构建议：

```json
{
  "version": 1,
  "status": "ok",
  "request_type": "offline_code",
  "applied_minutes": 30,
  "today_limit": 120,
  "dry_run": false,
  "mode": "grant",
  "state": {
    "limited_today": true,
    "blocked_today": false,
    "unrestricted_today": false,
    "remaining_available": true,
    "remaining_minutes": 65,
    "play_timer_enabled": true,
    "restricted_now": false,
    "today": 3,
    "date_key": 3335,
    "active_rule": "limit",
    "active_rule_minutes": 120,
    "bedtime_active": false,
    "parent_unlock_active": false
  },
  "capabilities": {
    "limit_action": "remind",
    "raw_block_verified": false,
    "suspend_verified": false
  }
}
```

错误结构建议：

```json
{
  "version": 1,
  "status": "error",
  "request_type": "offline_code",
  "reason": "bad_signature",
  "mode": "grant"
}
```

## 5. 数据流设计

### 5.1 离线码加时流程

```text
Parent Tool
  -> 生成 code
  -> 家长告诉孩子

Companion NRO
  -> 写 grant_request.json {type:"offline_code", code}
  -> 等待 grant_result.json

Sysmodule
  -> 读取并删除 request
  -> 加载 config/rules/state
  -> 检查 disable.flag/control_mode
  -> Auth Core 验证 code
  -> Control Policy 生成执行计划
  -> observe: 读取 PCTL 并返回 dry_run
  -> grant/enforce: 写备份，写 PCTL，成功后消费 nonce
  -> 写 events.jsonl/monthly_report.txt/grant_result.json

Companion NRO
  -> 读取 result
  -> 展示成功、错误和当前状态
```

### 5.2 家长区修改今日规则流程

```text
Companion NRO
  -> 密码校验
  -> 写 request，例如 set_today_limit

Sysmodule
  -> 更新 time_rules.json 今日 override
  -> 根据 control_mode 决定 dry run 或写 PCTL
  -> 写事件和结果
```

### 5.3 Bedtime 流程

```text
Sysmodule 周期刷新
  -> Time Rule Core 判断 bedtime 是否进入/退出
  -> parent unlock active 时不施加强控制
  -> limit_action=remind: 只写事件和状态
  -> limit_action=suspend/block 且验证通过且 enforce: 写 PCTL 限制
  -> bedtime 结束后恢复今日规则
```

### 5.4 Parent Unlock 流程

```text
家长开始 unlock
  -> 写 time_state parent_unlock_until
  -> stop play timer
  -> 写事件

Sysmodule 周期检查
  -> 当前时间超过 until
  -> 清除 unlock 状态
  -> start play timer
  -> 写事件
```

## 6. 状态机设计

### 6.1 Sysmodule 控制状态

```text
Booting
  -> ServicesReady
  -> ConfigLoaded
  -> DisabledByFlag
  -> RunningObserve
  -> RunningGrant
  -> RunningEnforce

任意状态 + disable.flag
  -> DisabledByFlag

DisabledByFlag 移除 disable.flag
  -> 下次请求或刷新时重新加载配置并恢复对应模式
```

### 6.2 请求处理状态

```text
Idle
  -> RequestFound
  -> RequestParsed
  -> RequestValidated
  -> PlanCreated
  -> ExecutedDryRun | ExecutedWrite | Rejected
  -> ResultWritten
  -> Idle
```

错误处理：

- parse 失败：`bad_request`
- unknown type：`unknown_request`
- disabled：`disabled`
- PCTL 失败：对应 `pctl_*` reason
- storage 失败：对应 `write_*_failed`

### 6.3 授权码状态

```text
Decoded
  -> VersionChecked
  -> ActionChecked
  -> SignatureChecked
  -> DateChecked
  -> NonceChecked
  -> LimitChecked
  -> Accepted
```

只有 `Accepted + PCTL write success` 才进入 `Consumed`。

## 7. 安全与恢复设计

### 7.1 默认安全配置

```json
{
  "control_mode": "observe",
  "allow_unlimited_to_limited": false,
  "raw_block_verified": false,
  "suspend_verified": false
}
```

### 7.2 Fail-open

检测到：

```text
sdmc:/switch/pctltcp-sysmodule/disable.flag
```

系统行为：

- 不读取 PCTL。
- 不写 PCTL。
- 不启用或刷新 play timer。
- 授权请求返回 `disabled`。
- 写日志说明 fail-open active。

### 7.3 写入前备份

所有 PCTL 写入前必须生成：

```text
last_pctl_backup.dat
```

建议内容：

```text
version=1
posix_time=...
control_mode=grant
operation=add_today_minutes
today=3
old_limited=true
old_limit=120
new_mode=limit
new_limit=150
raw_hex=...
```

### 7.4 高风险能力门控

raw `0` 禁玩：

- 默认请求 `block_today` 返回 `raw_block_not_verified`。
- 只有 `probe_raw_block` 在真机确认后才能设置 `raw_block_verified=true`。
- 普通 block 请求必须检查该开关。

Suspend：

- 默认只允许 remind。
- suspend 行为必须在真机验证后设置 `suspend_verified=true`。
- 建议仅在 `enforce` 模式生效。

## 8. Companion NRO 架构

### 8.1 UI 分层

```text
main loop
  -> input manager
  -> screen router
  -> home screen
  -> parent screen
  -> dialogs / keyboard
  -> request client
```

### 8.2 Home Screen

能力：

- 展示今日额度、剩余时间、当前状态。
- 输入离线码。
- 刷新状态。
- 查看最近结果。
- 隐藏家长入口：长按 `L + R + X`。

不应展示：

- grant_secret。
- 家长区入口明文按钮。
- 高风险功能说明给孩子主界面。

### 8.3 Parent Screen

进入条件：

- 本地密码通过。
- 默认密码 `1234`，缺失或空文件回退默认。

能力：

- 今日固定额度。
- 今日加时。
- 今日不限。
- 今日禁玩。
- 恢复周模板。
- 周模板。
- bedtime。
- parent unlock。
- 文件/事件/月报查看。
- 修改密码。

### 8.4 Request Client

职责：

- 写 `grant_request.json`。
- 等待 `grant_result.json`。
- 处理超时。
- 支持 status refresh。

建议：

- 写请求前删除旧 result，避免读到上一轮结果。
- request 带 `request_id`，result 回写同一个 `request_id`，避免竞态。
- 超时只代表 sysmodule 未响应，不代表请求一定失败。

## 9. 家长侧工具架构

### 9.1 CLI

命令：

```bash
python tools/grant_code.py \
  --minutes 30 \
  --device kid-switch \
  --secret change-me-to-a-long-random-secret
```

测试参数：

```bash
--date YYYY-MM-DD
--nonce 0x1234
--explain
```

建议输出：

```text
20F0-SSGJ-6JEA-AEJE
date_key: ...
nonce: ...
minutes: 30
```

默认第一行必须是短码，方便复制给孩子。

### 9.2 协议测试工具

新增 `tools/protocol_test.py`：

- 生成 request JSON。
- 注入模拟 result JSON。
- 校验 companion 可读取字段。
- 可在 PC 上验证 token 编解码和错误码。

## 10. 测试架构

### 10.1 单元测试

优先覆盖：

- Base32 编解码。
- token payload 打包/解包。
- HMAC 签名匹配与不匹配。
- 日期 key 计算。
- nonce ledger 查重。
- time rules 规则优先级。
- bedtime 跨天判断。
- control_mode 执行计划。
- request parser 错误处理。

### 10.2 Host-side 集成测试

用 mock PCTL adapter 跑请求处理：

- 有效码 dry run。
- 有效码真实写入 mock。
- 重复码拒绝。
- 错日期拒绝。
- 错密钥拒绝。
- 超上限拒绝。
- unlimited 保护拒绝。
- `disable.flag` 拒绝。
- parent unlock 过期恢复。
- bedtime remind/block 分支。

### 10.3 模拟器测试

验证范围：

- NRO 启动。
- 输入、软键盘、按键。
- 设置密码读写。
- request 文件写入。
- 手动 result 文件读取。
- 家长区请求生成。

不作为失败依据：

- sysmodule 日志不存在。
- PCTL 行为不可用。
- 请求等待超时。

### 10.4 真机分阶段测试

阶段：

1. NRO-only，不包含 boot2。
2. `disabled` boot2 冒烟。
3. `observe` 验证码和读取状态。
4. `grant` 1 分钟最小写入。
5. `grant` 拒绝用例。
6. `enforce` 强控制。
7. 家长区本地规则。
8. raw block probe。

每阶段记录：

- 输入码。
- 预期结果。
- 实际 `grant_result.json`。
- `sysmodule.log` 关键行。
- PCTL 是否变化。
- 是否生成备份。
- 是否可恢复。

## 11. 迁移与兼容策略

### 11.1 建议保留的兼容项

- 工作目录：`sdmc:/switch/pctltcp-sysmodule/`
- 核心配置：`grant.conf`
- 请求文件：`grant_request.json`
- 结果文件：`grant_result.json`
- 防重放文件：`used_grants.dat`
- 停用文件：`disable.flag`
- 备份文件：`last_pctl_backup.dat`
- `control_mode` 语义
- token 日期、nonce、HMAC 语义

### 11.2 可演进项

- request/result 增加 `version` 和 `request_id`。
- time rules 增加 schema version。
- result 中把状态拆到 `state` 和 `capabilities` 对象。
- 日志事件增加 stable `event_id`。
- used ledger 增加文件头版本。

### 11.3 迁移处理

新版本启动时：

- 如果文件没有 `version`，按 v0/v1 兼容解析。
- 写回时统一写新格式。
- 对未知字段保守忽略。
- 对未知请求类型返回 `unknown_request`。

## 12. 推荐实现顺序

1. 建立 `core/` 纯逻辑模块和 host-side 测试。
2. 实现 auth token、date key、ledger 和 control policy。
3. 实现 JSON codec、config store、atomic file。
4. 用 mock PCTL 完成 request dispatcher 集成测试。
5. 接入真实 PCTL adapter，但先只读状态。
6. 重建 companion request client 和主界面。
7. 实现家长区规则请求。
8. 接入 `observe` 真机测试。
9. 接入 `grant` 写入和备份。
10. 最后实现 `enforce`、bedtime 强控制、raw block probe。

## 13. 关键设计取舍

- 文件协议优先于进程间直接通信：更简单、可调试、适合 homebrew 和 sysmodule 边界。
- observe 是默认模式：牺牲一步启用便利性，换取真机安全。
- PCTL raw 写入集中封装：避免业务代码散落 raw offset，降低固件适配风险。
- 高风险能力显式验证：避免 raw block/suspend 在未确认机器行为时误伤系统状态。
- 核心逻辑 host-side 可测：Switch 真机测试成本高，必须把大部分错误提前拦在 PC 测试阶段。

