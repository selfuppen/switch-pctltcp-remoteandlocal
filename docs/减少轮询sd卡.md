可以减少，而且建议这么设计：**文件仍作为持久化协议，但不要高频轮询 SD 卡**。

更合理的方案是做成“低频轮询 + 事件化/节流”的混合模式：

1. **companion 主动写请求后等待结果**
   - NRO 写 `request.json`。
   - sysmodule 不需要每秒扫很多文件，只检查一个固定 inbox 文件。
   - NRO 等待 `result.json` 时可以短时间高频检查，比如 100-200ms，超时后停止。

2. **sysmodule 降低空闲轮询频率**
   - 没有请求时，每 5-10 秒检查一次 `request.json`。
   - 检测到请求后立即处理。
   - 连续处理后短时间进入 active window，比如 10 秒内每 250ms 检查一次，之后回到低频。

3. **用单个“信号文件”减少扫描**
   - companion 写：
     - `request.json.tmp`
     - rename 为 `request.json`
     - 可选再写 `wake.flag`
   - sysmodule 只检查 `wake.flag` 或 `request.json` 是否存在，不枚举目录。
   - 处理完删除 `wake.flag` 和 `request.json`。

4. **把定时任务和请求轮询分开**
   - 请求检查：空闲 5-10 秒一次，活跃期 250ms 一次。
   - bedtime/parent unlock 到期检查：按下一次到期时间计算，不用每秒查。
   - 心跳日志：60 秒或更久一次。
   - 配置重载：只在请求到来时加载，或每几分钟低频检查。

5. **如果愿意提高复杂度，可以用 IPC 替代请求唤醒**
   - companion 通过自定义 IPC 通知 sysmodule。
   - sysmodule 收到 IPC 后读取/处理内存请求或文件请求。
   - 这样可以基本消除轮询，但实现复杂度和真机调试风险会上升。

我建议在新架构里写成：

```text
Transport v1: File Inbox with Adaptive Polling
- idle_poll_interval_ms = 5000
- active_poll_interval_ms = 250
- active_window_ms = 10000
- result_wait_interval_ms = 100
- request file uses atomic rename
- no directory scan, only fileExists(request.json)
```

也可以进一步设计一个 `RequestTransport` 抽象：

```c
typedef struct {
    bool (*has_request)(void *ctx);
    bool (*read_request)(void *ctx, Request *out);
    bool (*write_result)(void *ctx, const Result *result);
    uint32_t (*next_wait_ms)(void *ctx);
} RequestTransport;
```

第一版用文件实现，后续如果要换 IPC，不动业务逻辑。