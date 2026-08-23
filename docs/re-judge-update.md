# libphira.so 逆向定位报告：Judge::update autoplay 分支（v1，2026-08-23）

样本：`Phira_0.8.2.apk`（`/sdcard/MT2/apks/`）→ `lib/arm64-v8a/libphira.so`
（26,522,936 字节）。工作副本：`/tmp/opencode/phira-so/lib/arm64-v8a/libphira.so`。
上游源码对照：本仓库 `prpr/src/judge.rs`。

## 1. 地址映射（已三方交叉验证）

ELF LOAD 段（`readelf -lW`）：

| 段 | 文件偏移 | 虚拟地址 | 大小 | 属性 |
|---|---|---|---|---|
| LOAD#1 | 0x000000 | 0x000000 | 0x6ec11c | R （含 .rodata/.dynsym）|
| LOAD#2 | 0x6ec200 | 0x6f0200 | 0x10d8a00 | **R E**（.text）|

换算公式：

```
.text:   file_off = vaddr - 0x4000          （delta 来自 0x6f0200-0x6ec200）
.rodata: vaddr == file_off                  （LOAD#1 零偏移）
运行期面板 span 偏移 = vaddr - 0x6f0000     （exec 段页对齐映射基址，实测校准）
```

校准证据：面板报 cand4 @span+0x60fff0 指令字 `37000269`；文件内该指令字唯一命中
4 处之一恰为 file_off `0xcfbff0` → vaddr `0xcffff0` = span+0x60fff0 ✓。

## 2. 判定常量与引用点（python adrp+ldr 扫描）

常量（.rodata，f64 LE）：

| 常量 | 字节 | rodata 地址 | 出现次数 |
|---|---|---|---|
| LIMIT_PERFECT=0.08 | 7b14ae47e17ab43f | 0x277d48 | 1 |
| LIMIT_GOOD=0.16 | 7b14ae47e17ac43f | 0x2783d0 | 2 |
| LIMIT_GOOD 副本 | 同上 | 0x279ea0 | ↑ |
| X_DIFF_MAX=0.21/(16/9)*2 | 3e0ad7a3703dce3f | 0x27bc50 | 1 |

与 LSPosed 面板 probes p0=1 p1=2 完全一致 ✓。代码引用点（adrp + ≤6 条内 ldr d/add）：

```
LIMIT_PERFECT : 0xdca578, 0xdcad9c        ← judge::update manual 主循环簇
LIMIT_GOOD_A  : 0xd00018(比较器), 0xdca56c, 0xdcad88, 0xdcb520, 0x164fa84
LIMIT_GOOD_B  : 0xdcaefc
X_DIFF_MAX    : 0x156c058
```

## 3. ★ 目标分支（最终结论）

**judge::update 函数**：入口 ≈ `0xdc9288`（前一函数 ret 在 `0xdc9278`，
llvm-objdump 反汇编确认序言 stp×6/sub sp）。

```asm
dc92b8: 39422028  ldrb w8, [x1, #0x88]   ; x1=res, res.config.mods
dc92bc..cc: 保存 x19/x20/x21/x28、res/chart 入参
dc92d0: 37000548  tbnz w8, #0x0, 0xdc9378   ; ★ autoplay ? 转 auto 体
; ---- fallthrough = manual 路径（对应 judge.rs:417 起）----
dc92d4: bd409ea0  ldr s0,[x21,#0x9c]      ; uptime 基准
dc92ec: 9427fb0d  bl clock_gettime@plt     ; get_uptime()（judge.rs:420）
dc931c/dc9324: bl …                       ; touches()/按键轮询（judge.rs:424+）
…（manual 主体含 0xdca5xx 的 PERFECT/GOOD 判定循环，Note 步长 32 字节）
; ---- taken 边 = 内联展开的 auto_play_update（judge.rs:873）----
dc9378: ldp x10,x9,[x28,#0x78]           ; chart.notes
dc93c4..94f0: 遍历 notes，cmp w11,#2/#3   ; NoteType 过滤
dc94b4: strb w8,[x27,#0x30]              ; 写 note.judged 状态
…（无 bl auto_play_update——LTO 已完全内联，故无调用形状）
```

源码对照（judge.rs:412-416）：`if res.config.autoplay() { self.auto_play_update(res, chart); return; }`

### Patch 方案

```
位置：vaddr 0xdc92d0（file 0xdc52d0，运行期 span 偏移 0x6D92D0）
原值：37000548  tbnz w8,#0,+0xa8
新值：1400002a  b    +0xa8            ；无条件跳同一目标，offset 不变
验证：(0xdc9378-0xdc92d0)/4 = 0x2A ✓；语义：mods 位无论 0/1 恒走 auto 体
效果：config.mods 保持 0（无 AUTOPLAY 标识），仅判定行为变为自动
```

## 4. 此前远程扫描失败根因（复盘）

1. **形状评分全灭**：auto 体是内联大块而非 `bl+b` 小块，`[bl..b]` 形状在真实目标上
   根本不存在；且旧版 `imm14_of` 移位错一位使 tbz 目标算错（v0.6.0 已修）。
2. **候选全非**：0xcffe44-0xd004b4 一带是 float 比较器两份拷贝（tbnz+b.eq 汇合型）
   与一个双路径合流函数，均非 judge。
3. **门控漏掉真目标**：真分支距最近常量引用点 4776B > XREF_WIN=2048，被滤除。

## 5. 复现命令

```bash
cd /tmp/opencode/phira-so
unzip -o "/sdcard/MT2/apks/Phira_0.8.2.apk" "lib/arm64-v8a/libphira.so"
readelf -lW lib/arm64-v8a/libphira.so | sed -n '1,25p'
# 目标区反汇编：
llvm-objdump -d --triple=aarch64-linux-android \
  --start-address=0xdc927c --stop-address=0xdc9520 lib/arm64-v8a/libphira.so
# 常量定位：
python3 - <<'EOF'
import struct
d=open('lib/arm64-v8a/libphira.so','rb').read()
print('PERFECT', hex(d.find(struct.pack('<d',0.08))))
print('GOOD', [hex(i) for i in [d.find(struct.pack('<d',0.16)), d.rfind(struct.pack('<d',0.16))]])
print('X_DIFF_MAX', hex(d.find(struct.pack('<d',0.21/(16./9.)*2))))
EOF
# adrp+ldr 引用扫描脚本见会话记录（targets 改用 rodata vaddr==file_off）
```

## 6. 后续动作

- [ ] phira_agent.c 增加 `PATCH_AT <span_off> <expect_insn>` 直写指令（带原值校验，
      不匹配即拒绝）；当前构建 profile：`PATCH_AT 6d92d0 37000548`
- [ ] 结构扫描加「函数入口距离」评分（真分支位于入口 +0x54B 内，噪声多在函数中部），
      作为未来轻改版本的自动定位兜底；XREF_WIN 默认提到 ≥8192
- [x] 登录/上传接口分析（phira/src/client.rs，见 docs/api-login-upload.md）

## 7. 永久存档（lsposed-autoplay/re/）

```
re/
├─ Phira_0.8.2/
│  ├─ Phira_0.8.2.apk                    原始 APK（41MB）
│  └─ lib/arm64-v8a/libphira.so          解包 so（26MB，本文所有地址基于此文件）
├─ disasm-judge-update.txt               dc927c-dc9530 反汇编（目标分支+内联 auto 体）
├─ disasm-manual-loop.txt                dca480-dca640 反汇编（manual 判定循环头部）
└─ tools/scan_xrefs.py                   adrp+ldr 引用扫描器（python3 直接跑可复现 §2 输出）
```

复现（在本目录）：`python3 tools/scan_xrefs.py`；反汇编命令见 §5。
换新版本 so 时：readelf -lW 更新脚本顶部 TEXT_OFF/TEXT_VA/TARGETS 即可。
