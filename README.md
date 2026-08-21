# phira-lsposed-test

仅用于本地测试的 LSPosed 模块：在非自动游玩模式下，让 Phira 走其**原生 autoplay 判定路径**，
但不携带任何 autoplay 标识（方向 1）。

> ⚠️ 合规边界：仅供**自编译测试构建 + 本地/自建服务端**使用。不得指向官方/生产服务器。

## 原理

Phira 全部游戏逻辑在 `libphira.so`（Rust cdylib）。原生 autoplay 由
`Judge::update` 入口的一个条件跳转控制（`prpr/src/judge.rs:412-416`，
`res.config.autoplay()` 即 `config.mods & 1`）。本模块在该进程内定位这条
条件分支并改写为无条件跳转（或 NOP），使判定走原装 `auto_play_update`；
由于 `config.mods` 未被改动：

- 游戏内无 "AUTOPLAY" 标识 / 模组图标 / 结算标记；
- 计分与本地成绩保存走正常管线。

## 弹性定位（不死偏移）

release 二进制已 strip 且随版本变化，模块不做任何死定位：

1. **结构扫描**：`tbz/tbnz`（测 bit0）或 `tst wn,#1` + `b.cond` 指令形态；
2. **指纹门控**：候选 ±WINDOW 字节内必须存在 prpr 判定常量 f64 字面量
   （LIMIT_PERFECT=0.08、LIMIT_GOOD=0.16，见 `assets/phira_profile.txt`）；
3. **极性判定**：cond 码启发式 + `[bl ... b]` 形态评分；猜错只会退化为
   always-manual（安全无效），不会崩溃；
4. **显式 PATTERN 兜底**：profile 支持带通配符的字节模式。

任一环节不确定 → **放弃，不写入任何字节**（候选数必须恰好等于 `MAX_CAND`）。

### 适配未来版本

无需改 C 代码，按优先级读取 profile：

1. `/data/local/tmp/phira_autoplay_profile.txt`（热更，免重装）
2. APK 内 asset `phira_profile.txt`（改完推 CI 重编即可）
3. 编译期内置默认

profile 语法见 `app/src/main/assets/phira_profile.txt` 头部注释。
新版本失效时的排查步骤：

```bash
# 在自编译产物上反汇编，找 Judge::update（特征：附近字面量池含 0.08/0.16 的 f64）
objdump -d --no-show-raw-insn libphira.so | less   # 搜索 "tbnz" / "tbz"
```

确认新形态后调整 `WINDOW/MAX_CAND/PROBE/PATTERN` 即可。

## 构建

编译完全由 GitHub Actions 云端执行（`.github/workflows/build.yml`）：
push 到 main 或手动触发 → Artifacts 下载 `phira-lsposed-test-debug.apk`。本地不构建。

## 使用

1. 安装 APK，在 LSPosed 中启用本模块；
2. 作用域勾选你的 Phira 测试包名；
3. 若包名不在白名单内，编辑
   `app/src/main/java/cn/test/phirauto/MainHook.java` 的 `TARGET_PACKAGES`
   后重新触发 CI；
4. 启动游戏，看到 Toast「Phira 方向1 注入成功」即表示分支已改写；
   定位失败时静默退出（logcat 过滤 `PhiraAgent` 可见原因），宿主不受影响。

## 工程

- Java 入口：`MainHook`（hook `Instrumentation.callApplicationOnCreate`，进程内仅触发一次）
- JNI 代理：`PhiraAgent.arm()` 阻塞轮询 `/proc/self/maps` 直到 `libphira.so` 出现，
  完成扫描→校验→4 字节 patch→复读校验（超时 120s）
- Xposed API 以编译期 stub 形式提供（运行时由 LSPosed 注入真实实现），零外部依赖
