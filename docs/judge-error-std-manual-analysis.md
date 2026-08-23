# 判断误差（std 无暇度）链路手动汇编分析（2026-08-23，objdump 层）

样本：`re/Phira_0.8.2/lib/arm64-v8a/libphira.so`（与桌面链接同一文件）。
工具：`aarch64-linux-gnu-objdump`。目的：确认"判断误差"从产生→累加→最终 std→
编码进载荷的整条数据链（GUI 反编译待用户确认函数边界）。

## 链路总览

```
手动循环（dca5xx）── 逐 note 算偏移 ──> 统计累加器（sp+80/88/104/112）
      │                                  │
      │ offset = |hit − note| / speed     ▼
      │                            统计收尾：std = √(Σx²/n)
      │                              （0xdb73a8 候选，RMS 模式）
      ▼                                  │
 判定分档（PERFECT/GOOD/MISS）      res 结构体 +0xCB8（f64 std）
      │                                  │
      ▼                                  ▼
                              encode_record（0xd24860）
                              读 res+0xCB8 → fabs → 定点化(2^14) → 载荷
```

## 1. 判断误差的产生（手动循环，两处）

**位置 A（0xdca840，滑键/位移类）**：
```
dca840: ldr  d0, [x19, #320]    ; 当前/实际时刻
dca844: ldr  d1, [sp, #248]     ; speed
dca848: fsub d0, d0, d12        ; offset = 实际 − note时间
dca84c: fdiv d0, d0, d1         ; ÷ speed 归一化
dca850: fcmp d0, d10            ; 与窗口阈值比
```

**位置 B（0xdca968，点按类）**：
```
dca948: bl   d60b08             ; 命中判定辅助
dca968: fabd s0, s0, s11        ; fabd = 绝对差 |hit − note时间|
dca96c: fcvt d0, s0
dca970: fdiv d0, d0, d1         ; ÷ speed（[x19,#380]）
dca974: fcmp d0, d9             ; 与窗口阈值比
```

分档阈值（循环头 dca560-5e0 从 rodata 装载）：LIMIT_PERFECT=0.08（0x277d48）、
LIMIT_GOOD=0.16（0x2783d0）、X_DIFF_MAX=0.23625（0x277c50）。

## 2. 累加器（统计原料）

偏移进入多个统计槽，fadd/fmaxnm/fminnm/fabs/fcsel 组合（0xdca858-87c、0xdca97c-9a8）：
```
dca97c: ldr d1, [sp,#88]    ; 累加器
dca988: fadd d1, d0, d1      ; acc += offset
dca990: fmaxnm d1, d1, d2    ; clamp ≥ 0
dca994: ldr d2, [sp,#80]
dca998: fmul d1, d1, d2      ; 加权
dca9a0: fadd d1, d1, d15     ; + 常数
```
（sp+80/88/104/112 = 权重/和/加权和/计数等，具体语义待 GUI 数据流确认。）

## 3. std 最终化（候选：0xdb73a8，RMS 模式）

```
db7394: fdiv d0, d9, d0      ; d0 = d9 / count
db73a8: fsqrt d0, d0         ; √(d9/count)  ← RMS 形态
db73ac: fcvt s0, d0
db73b0: str s0, [x19, #76]   ; 存 float
```
注：judge 区域（0xdc9000-0xdcd000）无 d 寄存器 fsqrt；最近双精度 fsqrt 在此。
**d9（sumsq）来源待 GUI 数据流确认**——此块前置大量整数一致性比对（w23/w27/...
vs sp+48 等），可能是统计收尾前的自检。

## 4. encode_record 序列化（0xd24860，6328B）

载荷构造关键点：
```
d24dbc: ucvtf d0, w21        ; count
d24dc4: fdiv d0, d8, d0      ; 聚合字段1 = d8/count（f64，均值类）
d24dc8: str  d0, [x8, x1]    ; → 输出缓冲
d24dd4: ldr  w19, [x20,#648] ; res+0x288（int 字段）
d24df8: ldr  d0, [x20,#3256] ; ★ res+0xCB8 = std（f64），fabs 后定点化
d24e00: fabs d0, d0
d24e0c: fcvtzu x19, d0, #14  ; 定点化 2^14（1/16384 秒精度）
（后接取整位运算 + bl 1639ca8）
```
- **res+0xCB8 是 std 字段**：编码器只读不算（整个 encode_record 无 fsqrt）；
- 定点化 2^14 ≈ 0.061ms 分辨率 → "无暇度"上报精度；
- 载荷短 = 聚合字段为主（印证用户抓包"请求体极短"）。

## 5. 与注入（auto 路径）的关系

auto 体（0xdc9378+）判定逻辑：
```
dc94b0: mov w8, #2           ; 固定判定值
dc94b4: strb w8, [x27,#0x30] ; 写 note 状态，完
```
**不计算偏移、不更新任何统计累加器** → sumsq/count 保持 0 → std=√(0/n)=0
→ res+0xCB8=0 → 载荷"完美无暇"（std≈0）→ 服务端下限检查拒收（实测吻合）。

## 6. 结论与待 GUI 确认项

- 已确证：误差公式（|hit−note|/speed）、分档阈值、encode_record 只读序列化
  res+0xCB8、auto 路径完全跳过误差链（std=0 根源）；
- 待 GUI 确认：①累加器字段语义（sum/sumsq/count 具体分配）；②0xdb73a8 的
  d9 数据流（是否即 std 最终化，还是另一子系统）；③res+0xCB8 在 judge 侧
  的写入点（std 何时落位）。
