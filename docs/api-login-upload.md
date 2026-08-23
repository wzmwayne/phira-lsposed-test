# Phira 登录与成绩上传接口分析（源码级，2026-08-23）

对照仓库：本 repo（Phira 上游源码）。HTTP 客户端：`phira/src/client.rs`（reqwest）。

## 1. 基础

| 项 | 值 |
|---|---|
| 生产 API | `https://phira.5wyxi.com`（client.rs:26）|
| 本地调试 | `http://localhost:2924`（client.rs:25，注释态，可切换）|
| 认证方式 | `Authorization: Bearer <token>`（client.rs:47-58，构建默认头）|
| 令牌存储 | `get_data_mut().tokens = Some((token, refresh_token))`（store_login，client.rs:312）|
| 证书 | `accept_invalid_cert` 可开 `danger_accept_invalid_certs`（自建服务端友好）|
| 语言头 | `Accept-Language` |

## 2. 账号相关端点

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/register` | body `{email, name, password}`（client.rs:262-271）|
| POST | `/login` | `LoginParams` flatten + `{clientVersion}`；响应 `{id, token, refreshToken}`（camelCase）（client.rs:275-310）|
| POST | `/login/hykb` | 好游快爆 SDK：`{hykbUid, accessToken}` → 直登或发短时 hykb_token（cfg(hykb)，client.rs:323+）|
| GET | `/me` | 当前用户信息（client.rs:415-418）|

资源列表通用前缀（model 的 `QUERY_PATH`）：`/user`、`/chart`、`/collection`、
`/event`、`/records`；详情 `GET /{path}/{id}`，分页 `GET /{path}?...`
（client.rs:236/518）。

## 3. 成绩上传链路（关键门控）

`prpr/src/scene/game.rs:949-957`（State::Ending 收尾）：

```rust
#[cfg(closed)]                                    // ← OSS 开源构建整段编译剔除！
if let Some(upload_fn) = &self.upload_fn {
    if !res.config.offline_mode
        && !res.config.mods.intersects(Mods::UNRATED)   // UNRATED mod 挡上传
        && !res.config.use_keyboard
        && res.config.speed >= 1.0 - 1e-3
    {
        record_data = Some(encode_record(self, player.id, *chart));
    }
}
```

- **开源构建（本仓库）没有 `encode_record` 实现**（cfg(closed)），`upload_fn` 为 None：
  天然无上传路径。
- 本地 best 记录仍走 `save_fn` + `SimpleRecord{score, accuracy, full_combo}`
  （game.rs:963-973），UNRATED 或变速时 record=None 不保存。
- 编码细节在闭源部分；服务端另有 `POST /record/best/{chart_id}` 查询个人最佳。

## 4. 文件上传

```
POST /upload/{name}   body=原始字节   响应 {id}
```

（client.rs:428-435，用于头像/谱面等）

## 5. 与本项目（autoplay 测试模块）的关系

- 方向 1 patch 只改 judge 判定路径，不动 config.mods → `Mods::AUTOPLAY` 位为 0，
  不触发任何「作弊标识」逻辑；成绩是否上传取决于所连服务端与构建形态。
  **仅限本地/自建服务端测试**（AGENTS.md 合规边界）。
- 自建服务端联调：改 client.rs:25 的 API_URL 即可。
