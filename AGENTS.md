# AGENTS.md — 项目记忆（实时更新）

## 任务上下文

用户在分析 Phira（Rust 节奏游戏，本仓库为其开源源码）并构建一个**仅用于本地测试**的
LSPosed 模块。已确认采用「方向 1」方案，其余方向（方向 2 触控模拟）已否决。

## 项目终态（2026-08-23，用户决定"就到这里"）

**目标已达成，项目进入休眠**：v0.7.1 注入已验证生效（本地演示用），上传链路结论
已定案（不破坏），双仓库已私有/归档，公开调研已存档。不再投入开发；如需重启，
按 AGENTS.md 记忆即可恢复上下文。遗留低优先级项（见下）不再处理。

## 方向 1 方案定案（勿再更改）

- **目标**：patch `libphira.so` 中 `Judge::update` 入口的 autoplay 条件跳转
  （`prpr/src/judge.rs:412-416`），使其无条件走原生 `auto_play_update`。
- **关键约束**：不改动 `config.mods` → 无 AUTOPLAY 标识、计分/本地保存走正常管线。
- **定位策略（演进后现状）**：
  1. **PATCH_AT 直写（v0.7.0 新增，当前构建的正解）**：profile 指令
     `PATCH_AT <hex span偏移> <hex expect指令> [hex 替换]`；写入前校验原值，
     不符即拒绝（fail-safe）；省略替换则自动推导 tbnz→无条件 B 同目标；
  2. 结构扫描兜底：tbz/tbnz bit0 / tst+b.cond 形态 + adrp/ldr 常量引用点门控
     （XREF_WIN 默认 8192）+ 评分选择。**注意：对本构建无效**——真分支距最近
     常量引用点 ~70KB（judge::update 巨大），任何合理 XREF_WIN 都够不到；
  3. profile 热更优先级：`/sdcard/Documents/phira_autoplay_profile.txt`（v0.7.1 起，
     旧 `/data/local/tmp/phira_autoplay_profile.txt` 已废弃删除） > APK asset > 内置默认；
     内置默认（profile_defaults）已含当前构建 PATCH_AT 兜底，asset 加载失败也能注入；
  4. Fail-safe 哲学不变：歧义绝不写入。
- **合规边界（与用户确认过）**：仅自编译测试构建 + 本地/自建服务端；OSS 构建
  rated=false 无 encode_record（cfg(closed)），无上传路径；不得指向官方服务器。

## 工程状态

### 仓库与 CI
- https://github.com/wzmwayne/phira-lsposed-test ，main 分支，嵌套独立 git 仓库于
  `/home/wayne/date/phira/lsposed-autoplay/`。
- 当前 HEAD = **v0.7.1 (versionCode 10)**（PATCH_AT 内置兜底 + 三路来源回报），CI 成功
  （1m23s），artifact 名 phira-lsposed-test-debug。
- CI：setup-java 17 + gradle 8.7 + setup-gradle@v4 缓存 + ndkVersion 钉 26.3.11579264。
- 关键历史 commits：8e7dbac(stub 外移 :stub compileOnly)、d55451f(面板直报)、2430eb4(cbz 形态)、
  e640240/38a9f1d(xref 门控)、dd7c5ca(v0.5.1 精确偏移 xref+缓存+排名)、147df09(v0.6.0 imm14 修复)。

### 版本迭代史（远程扫描阶段，现已由逆向终结）
- v0.4.x~v0.5.1：详见 git log 与下文实测数据链；核心结论：常量在 .rodata 不在 X 段 →
  字面量门控失效 → 改 adrp+ldr 引用点门控；精确偏移后 sites=6/6。
- v0.5.1 实测：cand=41 全 score=2 tie → abort；两对相同 tbnz+b.eq 相距 ~96B。
- v0.6.0（147df09）：修复 imm14_of 移位错一位(bits[17:4]→bits[18:5])——tbz 目标全错、
  形状评分死因之一；backward 分支过滤(41→24)；组选择策略(同分同惯用法≤4 全 patch)。
- v0.6.0 实测：tgt 已正确(+28/+16 等)，但 st/sf 仍全 0 → 发现 resolve_auto_side_ex 对
  tbz/bcond 早返回根本没算形状 → v0.6.1 本地修：side_call_ret_shape(bl 前6条内+随后无条件b,
  16条窗口)，形状对所有候选计算。仍未发版即被逆向路线取代。
- v0.7.0（7e178d7）：
  - PATCH_AT 直写指令（见上）；patch_word() 从 rewrite_branch 抽出复用(mprotect RW→写→
    clear_cache→恢复RE→复读校验)；
  - entry_bonus()：向前找最近 ret(d65f03c0)+其后6条内有 prologue(sub sp 掩码 D10003FF/
    stp x29,x30 低16==7BFD 高8==A9)；距入口≤512B 得2分、≤4096 得1分；
  - modload_bonus()：分支前8条内有 ldrb imm12((w>>22)==0x0E5) 得1分（对应 ldrb w8,[x1,#0x88]）;
  - 总分=heuristic(2)+形状互斥(1)+entry(≤2)+modload(1)，组选择阈值提升为 ≥4；
  - MAX_CANDS 64→128；XREF_WIN 默认 2048→8192；
  - 悬浮窗标题右侧中文状态标签：JNI status()→PhiraAgent.reportStatus→OverlayLog.status
    （青色 4DD0E1，标题与关闭按钮之间，attach 时恢复 lastStatus，初始"待机"）；
    状态词：自检/等待so/定位段/扫描中/未找到候选/计算评分/校验注入点/注入成功/注入+偏移/
    校验不符/越界放弃/写入失败/数量不符/歧义放弃/已放弃/超时/未找到so。

### ★ 本地逆向突破（2026-08-23，样本 Phira_0.8.2.apk）【最重要】
**用户环境就是手机上的 proot Linux**，`/sdcard/MT2/apks/Phira_0.8.2.apk` 可直接读！
解包到 /tmp/opencode/phira-so/，工具齐全：llvm-objdump/readelf/r2/python3。
- **地址映射（三方交叉验证）**：
  - LOAD#1 R：file_off == vaddr（0x0-0x6ec11c，含 .rodata/.dynsym）；
  - LOAD#2 R E：file 0x6ec200 / vaddr 0x6f0200 / 大小 0x10d8a00（.text）；
  - .text：file_off = vaddr - 0x4000；**面板 span偏移 = vaddr - 0x6f0000**；
  - 校准证据：面板 cand4 @span+0x60fff0 指令字 37000269 ↔ 文件内唯一命中 file 0xcfbff0
    → vaddr 0xcffff0 ✓。
- **常量地址（rodata，勿再加 0x4000！）**：LIMIT_PERFECT 0.08 @0x277d48（全文件唯一）；
  LIMIT_GOOD 0.16 @0x2783d0 + @0x279ea0；X_DIFF_MAX=0.21/(16/9)*2=0.23625 @0x27bc50。
  与面板 p0=1 p1=2 吻合。
- **引用点（re/tools/scan_xrefs.py 可复现）**：PERFECT: 0xdca578, 0xdcad9c；
  GOOD_A: 0xd00018, 0xdca56c, 0xdcad88, 0xdcb520, 0x164fa84；GOOD_B: 0xdcaefc；
  X_DIFF_MAX: 0x156c058。扫描器要点：SIMD 浮点加载 opcode 是 (w>>22)==0x3F5(ldr d)/
  0x3D5(ldr s)，GPR 是 0x3E5；adrp 判定位31==1 且 bits28..24==10000；imm=sx21<<12。
- **★最终目标**：judge::update 函数入口 ≈0xdc9288（前一函数 ret @0xdc9278）：
  ```
  dc92b8: 39422028  ldrb w8,[x1,#0x88]     ; res.config.mods（x1=res 第二参）
  dc92d0: 37000548  tbnz w8,#0,→0xdc9378   ; ★autoplay 分支
  fallthrough=clock_gettime(get_uptime)/touches() manual 路径(judge.rs:417+)
  taken 边=LTO 完全内联的 auto_play_update（notes 遍历/cmp #2 #3 NoteType/strb [x27,#0x30]
  判定状态写）——没有 bl 调用对，一切调用形状评分注定失败！
  manual 主体=0xdca5xx 判定循环（Note 步长32字节，PERFECT/GOOD 常量簇）。
  ```
- **Patch 定案**：span 偏移 **0x6D92D0**（vaddr 0xdc92d0/file 0xdc52d0）：
  `37000548` → `1400002a`（B +0xa8，同一目标；(0xdc9378-0xdc92d0)/4=0x2A ✓）。
- so 有少量导出符号（quad_main@0xb3cf28、Java_quad_1native_*、encode_record、jni_on_load、
  Java_moe_mivik_inputbox_InputBox_inputCallback），中间全是无符号内部函数，符号归属有欺骗性。
- 详细报告：lsposed-autoplay/docs/re-judge-update.md（§5 复现命令、§7 存档清单）。
- **永久存档 lsposed-autoplay/re/**：Phira_0.8.2/{Phira_0.8.2.apk 41MB,
  lib/arm64-v8a/libphira.so 26MB}、disasm-judge-update.txt(dc927c-dc9530)、
  disasm-manual-loop.txt(dca480-dca640)、tools/scan_xrefs.py（换版本需改脚本顶部
  TEXT_OFF/TEXT_VA/TARGETS）。
- **接口分析 docs/api-login-upload.md**：API_URL client.rs:26=https://phira.5wyxi.com
  （:25 注释态 localhost:2924 可切）；Bearer token 认证(client.rs:47-58)；
  POST /register {email,name,password}(:262)；POST /login LoginParams+clientVersion →
  {id,token,refreshToken} camelCase(:275-310)；POST /login/hykb(cfg(hykb))(:323)；
  GET /me(:415)；QUERY_PATH=/user /chart /collection /event /records；
  GET /record/best/{id}(:420)；POST /upload/{name} body原始字节→{id}(:428)；
  上传门控 game.rs:949-957：**#[cfg(closed)] 整段 OSS 构建剔除** +
  offline_mode/UNRATED/use_keyboard/speed≥1.0 检查；本地 best 走 save_fn+SimpleRecord。

### v0.7.1（✅ 已装机验证：注入生效）【已解决】
- 背景（v0.7.0 实测）：面板日志 `ats=0` → PATCH_AT 未生效，扫描路径 cand=71 全 tie
  → abort(-3)；日志值恰等于内置默认 → 疑似 asset 根本没被加载。目标 span+0x6d92d0
  不在候选中（距引用点~70KB，预期内，扫描路线对本构建本就无效）。
- v0.7.1 已完成的修复（commit c9c4b46）：
  1. PROFILE_OVERRIDE → `/sdcard/Documents/phira_autoplay_profile.txt`，旧
     `/data/local/tmp` 路径删除（用户明确指示）；
  2. profile_load 三路来源显式回报面板：`source=override/asset/builtin`；
     override fopen 失败带 strerror；AAssetManager_fromJava/AAssetManager_open/
     malloc/bad length 各打原因（含 LOGE）——为 asset 加载失败根因留证据；
  3. profile_defaults() 内置 PATCH_AT 6d92d0 37000548（repl=0 → derive 0x1400002a），
     任何情况下（asset 加载失败）都能注入；
  4. ★PATCH_AT 按 offset 去重（profile_line）：内置+asset 同条目 → 第二次校验不符
     abort 的陷阱，v0.7.1 特有，已修；
  5. versionCode 10 / versionName 0.7.1。
- 验证结果（用户确认）：**注入生效**——面板 `校验注入点` → `注入成功`、arm status=1、
  进局自动打。CI 两次构建全绿（1m50s / 1m25s）。
- ★安全措施（2026-08-23）：**仓库已设为 PRIVATE**（gh repo edit --visibility private），
  防止注入手法被他人利用；后续 push 走私有 Actions（免费额度内）。
- 未解遗留（低优先级）：asset 加载失败根因（疑 MainHook 传 assets 或 AAssetManager_open）
  未深挖，内置兜底已绕过；来源回报日志可供后续排查。

### ★ 上传逻辑影响结论（2026-08-23 用户问询，已定案）
- **机械层面：上传逻辑不被破坏**。patch 仅改写 judge::update 入口 1 条分支
  （tbnz→B，4 字节）；上传链路（game.rs Ending 门控、client.rs 网络/token/endpoint）
  零改动；config.mods 不写 → AUTOPLAY 位=0，无作弊标记。
- **构建形态**：OSS 构建（自编译 hacked 包）`#[cfg(closed)]` 剔除 encode_record →
  **本就没有上传路径**，无可破坏；本地 best 照常 save_fn 保存。闭源构建上传机制
  原样可用。
- **语义副作用（固有，非 bug）**：auto 体给所有 note 写固定判定值 2（dc94b0 mov w8,#2
  → dc94b4 strb [x27,#0x30]），成绩按正常计分；因 mods 无 UNRATED/AUTOPLAY，Ending
  门控全过 → 自动成绩会以**真实成绩形态**保存（OSS 本地 best）/上传（闭源连自建服）。
  这是方向 1「不改 mods」的固有结果，演示连自建服务端属预期。
- **设计边界**：patch 对进程是**全有全无**——装上即每次进局都自动打，无按局切换；
  需要"真玩+上传"时须在 LSPosed 停用模块或换未 patch 安装。
- **★本地闭源服务端上传实测（2026-08-23 用户自测，H1 已坐实）**：用户本地部署官方
  闭源服务端做上传测试——注入局网络上报"完美无暇"成绩（全判定2 + std≈0），但查询
  接口无记录；**对照实验：不注入正常打（含 AP 全 perfect）能查到记录**。
  → 排除 H2（chart 资格）/H4（格式版本不匹配）；**结论：服务端按 std（无暇度/逐
  note 时序）做人类合理性校验，std≈0 判机器产物直接拒收**。手动 AP 仍有真实打击
  偏移（std>0），注入的 auto 体写固定判定不产生时序（std≈0），指纹无法绕过。
  与非官方 API 文档中 std 字段为反作弊信号完全吻合。含义：官方服务端对自动成绩
  污染榜单有实际防线；注入手法即使泄露连官方服也会被拒收，合规风险进一步降低。
  不 coach 绕过服务端校验（越合规边界）。
- **★反作弊强度评估（2026-08-23 用户问询）**：std≈0 检查是**威慑层而非密码学边界**。
  本模块本质是进程内原生代码执行，恶意改造可 hook/改写上传载荷或判定时序来伪造方差，
  但需逆向闭源记录格式（encode_record）或 note 时序字段（工程量 ≥ 现有注入），且须
  伪造统计合理的分布（与谱面难度/判定分布一致，非随机数）。服务端真正防线：交叉
  一致性校验（std vs 判定分布 vs 谱面难度）+ 超低 std 顶分人工复核 + 防重放/频率
  限制。**模块保持私有 = 主要防线**。不 coach 绕过实现。
- **★上传载荷极短（2026-08-23 用户抓包，修正评估）**：上传请求体极短 → 估计只含
  聚合字段（以 std 为主），**不含逐 note 时序** → 服务端无法逐 note 校验，只能靠
  聚合合理性 + 行为侧 + 人工复核；伪造门槛**比我此前评估更低**（无需逆向完整
  encode_record 格式）。概念方向已存档 `docs/std-forgery-analysis.md`（私有仓库内，
  合规边界：不得用于官方服务器；不给可执行代码）。
- **★统计更正（2026-08-23 用户指出）**："全 perfect 配中等 std 不矛盾"——perfect
  窗口 ±0.08s，σ≤20~25ms 时 AP 是顶尖玩家正常画像（1000-note 必然/高概率 AP）；
  真正矛盾只在两端：std≈0（非人）或 std≳40ms 配 AP（统计不可能）。→ 现实伪造参数
  即 σ≈15~25ms + 全 perfect，**聚合层面与真人不可区分**；服务端 std 下限检查只拦
  std≈0 的现状模块，剩余有效防线是行为侧（账号历史/跨记录一致性）+ 顶分人工复核。
- **公开网络调研（2026-08-23，docs/web-research-crack-vuln.md）**：全网**无** Phira 成熟
  外挂/作弊器、无 CVE/PoC/RCE。相关公开物：phira-customizer（资源"加密"=无密钥可逆
  混淆，谱面可自由导出）、非官方 API 文档（端点全图 + std 无暇度字段=反作弊信号）、
  phira_ipa 等 iOS 重打包、下载站"修改版"（低可信）、phira-mp 自建服生态（无反作弊）。
  上游 security 标签 issue 均琐碎。用户本人公开的 phira-autoplay-rated（已放弃）是
  全网唯一 "autoplay 计 rated" 源码实现，建议私有化/删除。

### 其他环境事实（重要）
- 用户手机：华为 ROM（API 93），LSPosed；目标包 org.flos.phira.hacked（自编译 hacked 构建）
  与原版 org.flos.phira 二进制特征一致。
- /data/local/tmp 在 proot 内不可见不可写；/sdcard 直接可读。
- MT管理器 apk 库在 /sdcard/MT2/apks/。
- CI artifact 25KB 属正常（native lib 压缩率高）。
- 面板 abort 行每 200ms 重复刷屏至 latch(10轮)——已知小噪音，未修。

## 环境备注

- 工作目录：`/home/wayne/date/phira`（Phira 上游仓库本体，模块是其中嵌套的独立 git 仓库）
- 目标包名白名单已在 v0.3.0 移除门槛（用户包名 org.flos.phira.hacked）
- 用户偏好：中文交流、节奏快、不要多余中断；重要决策先问一次即可；要求实时更新本文件
