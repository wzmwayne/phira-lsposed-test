# Phira 破解/漏洞公开网络调研（2026-08-23）

范围：web_search + GitHub API（仓库/issue/commit 检索）。结论先行：

> **公开网络上不存在针对 Phira 的成熟外挂/作弊器，也没有已披露的真实安全漏洞**
> （无 CVE、无 PoC、无 RCE 类报告）。发现的公开内容集中在 6 个方向，见下。

## 1. 资源"加密"破解（最接近"破解"的公开物）

[wiorigami/phira-customizer](https://github.com/wiorigami/phira-customizer)（GPL-3.0，
单文件 `phira_codec.py`，依赖仅 zstandard）：

- Phira `assets/res` 的"加密"实为**无密钥的 8 字节块变换（置换+差分，可逆）** 包裹 zstd：
  交换字节 1↔4、2↔6；位逆序置换交换 (1,4) 与 (3,6)；三层蝴蝶加减 mod 256；交换 3↔5；
  尾部按「zstd 帧长度 = n + 末字节 - 8」截断/填充。
- 结论：资源层是**混淆不是加密**，任何图片/音频/谱面可自由加解密 → 谱面可导出、
  移植、二次分发。作者标注"全项目 vibe cod"。

## 2. 服务端接口面（非官方文档，逆向自 phira-web 源码）

[Phira API 非官方文档](https://www.xuziyao.com/posts/9/)（基于
[TeamFlos/phira-web](https://github.com/TeamFlos/phira-web)，Vue，3★）：

- 两个 API 域：`https://api.phira.cn` 与 `https://phira.5wyxi.com`
- 端点地图：`/chart`、`/chart/{id}`、`/chart/multi-get`、`/chart/{id}/stabilize-status`、
  `/event`、`/user`、`/user/{id}/stats`、`/record/get-pool/{id}`（Best20）、
  `/record/{id}`、`/record/multi-get`
- 游玩记录字段：`score/accuracy/perfect/good/bad/miss/max_combo/full_combo/mods/speed`，
  以及 **`std`（无暇度=打击偏移，秒）**、`std_score` —— 反作弊潜在信号字段
- 用户角色位掩码公开：USER=0 / ADMIN=1 / REVIEWER=2 / SUPERVISOR=4 / HEAD_SUPERVISOR=8 /
  HEAD_REVIEWER=16 / PECJAM_REVIEWER=32 / MODERATOR=64

## 3. 非官方分发 / 重打包（"破解版"生态）

- [ParaN3xus/phira_ipa](https://github.com/ParaN3xus/phira_ipa)（68★，2023-02 创建）：
  iOS IPA 非官方重打包；上游 README 中原本列出的
  [F-Unction/phira_ipa](https://github.com/F-Unction/phira_ipa) 已被划线移除
- 下载站"修改版"（低可信、SEO 农场特征，内容混入 Phigros 描述，宣称
  "解锁全部曲目/皮肤"）：[iefans](https://www.iefans.net/soft/v1706009.html)、
  [52pk](https://xin.52pk.com/game/223828.html)、[diyiyou](https://m.diyiyou.com/xiazai/390519.html)、
  [yxdown](https://www.yxdown.com/softview/xinsoftview_32820.html) 等
- [616.sb mirror](https://616.sb/#phira)：上游 README 官方列出的安卓镜像

## 4. 自建多人服务器生态（无内置反作弊）

- [TeamFlos/phira-mp](https://github.com/TeamFlos/phira-mp)（Rust，95★）：官方多人
  服务器/客户端库，开源
- [afoim/fuwari phira-mp 搭建教程](https://github.com/afoim/fuwari/blob/main/src/content/posts/phira-mp.md)
  + [afoim/phira-mp-autobuild](https://github.com/afoim/phira-mp-autobuild)（CI 自动构建）
- [chuzouX/phira-mp-nodejsver-*](https://github.com/chuzouX/phira-mp-nodejsver-websocket)：
  第三方 Node.js MP 服务器（管理面板/封禁/联邦路由）
- 未检索到公开的 MP 作弊工具或利用代码

## 5. autoplay 生态（合法用法）

- bilibili 专栏 [《关于如何用Phira与Autoplay录制打击音》](https://www.bilibili.com/read/cv25720024/)：
  autoplay 用于录制打击音/谱面演示
- [什么叫自动打歌](https://www.bilibili.com/video/BV11V4y1p7GL/) 等视频（演示向）

## 6. 上游 issue：无真实安全漏洞报告

- TeamFlos/phira 带 security 标签仅 3 个，均琐碎：
  - [#780](https://github.com/TeamFlos/phira/issues/780) 导入 respack 失败修复
  - [#25](https://github.com/TeamFlos/phira/issues/25) Windows 登录只能用剪贴板粘贴（UX）
  - [#731](https://github.com/TeamFlos/phira/issues/731) Windows ARM 缺 libunwind.dll
- [#593](https://github.com/TeamFlos/phira/issues/593) HarmonyOS 卓易通闪退，非安全
- phira-mp 未检索到 cheat/hack 类 issue

## 7. 用户自己的公开尝试（作者即仓库主，已放弃）

[wzmwayne/phira-autoplay-rated](https://github.com/wzmwayne/phira-autoplay-rated)（0★，
2026-08-03 创建，用户本人上传，已放弃）——公开网络上**唯一**的 "autoplay 计 rated"
实现：

- 核心改动（commit `3551d491`）：
  - `prpr/src/config.rs`：`UNRATED` 组去掉 `AUTOPLAY` 位（`Self::AUTOPLAY.bits() | ...` → 仅 NO_SHADER）
  - `prpr/src/scene/ending.rs` / `game.rs`：UI 去掉 "AUTOPLAY" 字样、恒显 "COMBO"
- 另有 CI 工作流"替换 APK 内 libphira.so + 重签名"（commit `623036c4`）——典型破解分发手法
- 若担心公开性：可设为私有或删除（与本次仓库私有化一致）

## 与本项目（LSPosed 注入路线）的相关性

1. **手法独特性**：`phira + lsposed/xposed/autoplay` 全网无第三方同类结果 → 注入路线
   目前是"独家"，仓库私有化决定正确。
2. **资源混淆可破**（phira-customizer）：谱面/资源版权保护形同虚设，但**不涉及成绩**，
   与作弊无关。
3. **接口面公开**：API 文档已公开完整端点地图 → 若官方服务端有鉴权/风控缺陷，攻击面
   是公开知识（与我们无涉，合规边界禁连官方服）。
4. **std 字段是反作弊信号**：自动判定写固定值 → `std≈0` 可被服务端检测。对"演示连自建服"
   无影响；若有人沿用本项目思路连官方服，会被 std 暴露（本项目合规边界已禁止）。
