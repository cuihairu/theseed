# Gateway — 网关层与登录安全

> theseed 内建网关：开箱即用，不需要额外部署 Nginx/Envoy。
>
> 来源：BigWorld LoginApp（简单代理），KBEngine LoginApp（TCP 转发）。
> theseed 新增 TLS、限流、路由、WebSocket 支持和 Challenge 登录防护。

---

## 1. 整体架构

```
Client
  │
  ├─ TCP (游戏协议) ──┐
  ├─ WebSocket ───────┤
  └─ HTTP (REST API) ─┤
                       │
                ┌──────▼──────────┐
                │    Gateway       │
                │  ┌────────────┐  │
                │  │ TLS 终结    │  │
                │  └─────┬──────┘  │
                │  ┌─────▼──────┐  │
                │  │ 认证层      │  │  ← Token / Session / HMAC
                │  └─────┬──────┘  │
                │  ┌─────▼──────┐  │
                │  │ 限流层      │  │  ← 令牌桶 / 滑动窗口
                │  └─────┬──────┘  │
                │  ┌─────▼──────┐  │
                │  │ 路由层      │  │  ← UID hash → BaseApp
                │  └─────┬──────┘  │
                └────────┼─────────┘
                         │
                  转发到内部进程
```

---

## 2. 边界说明

```
Gateway 负责：
  - 外部连接接入
  - TLS 终结
  - 基础认证握手
  - 限流与路由

Gateway 不负责：
  - 登录统计平台
  - 封禁列表运营策略
  - 在线运维命令
  - 分布式状态观察
```

也就是说：

```
Challenge 握手属于 Gateway / Login 接入边界
challenge 配置热改、ban 清理、状态检查属于 Ops Control Plane
```

---

## 3. 核心接口

```cpp
class IGateway {
public:
    virtual void start(const GatewayConfig& config) = 0;
    virtual void stop() = 0;
    virtual void setRouter(std::shared_ptr<IRouter> router) = 0;

    virtual size_t activeConnections() const = 0;
    virtual void kickConnection(ConnectionId id, const std::string& reason) = 0;
    virtual void kickAll(const std::string& reason) = 0;

    virtual void setRateLimit(const RateLimitConfig& config) = 0;
    virtual GatewayStats getStats() const = 0;
};

class IRouter {
public:
    virtual ServerEndpoint route(const AuthInfo& auth) = 0;
    virtual void updateRouteTable(const std::vector<ServerEndpoint>& endpoints) = 0;
};
```

---

## 4. 配置示例

```yaml
gateway:
  listeners:
    - protocol: tcp
      port: 20000
    - protocol: websocket
      port: 20001
    - protocol: http
      port: 20002

  tls:
    cert_file: /etc/theseed/certs/server.pem
    key_file: /etc/theseed/certs/server.key

  auth:
    method: token
    token_header: "X-Theseed-Token"

  rate_limit:
    global: 100000
    per_ip: 100
    per_second: 1000
    message_per_second: 100

  router:
    method: consistent_hash
    hash_key: uid
    virtual_nodes: 150
```

---

## 5. 登录安全

> 来源：BigWorld Login Challenge (Cuckoo Cycle PoW)。
> KBEngine 只有 Blowfish + rndUUID。
> theseed 借鉴 BigWorld 的 PoW 反 DDoS 思路。

### 5.1 多层防护

```
Layer 1: 网络层 — TLS 终结 + IP 限流 + 黑白名单
Layer 2: Challenge-Response — PoW 消耗客户端计算资源
Layer 3: Token 认证 — JWT token + 过期时间
Layer 4: 会话绑定 — IP/设备指纹绑定（可选）
```

### 5.2 Challenge 机制

```cpp
class ILoginChallenge {
public:
    virtual Challenge generate(const std::string& clientIp) = 0;
    virtual bool verify(const Challenge& challenge,
                        const ChallengeResponse& response) = 0;
    virtual void adjustDifficulty(float serverLoad) = 0;
};

// 简单 PoW：SHA256 前导零
class HashPoW : public ILoginChallenge {
    // 正常客户端 < 100ms，攻击者需要大量计算
};
```

### 5.3 对比

| 维度 | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| 登录 Challenge | Cuckoo Cycle PoW | 无 | HashPoW（可插拔） |
| 加密 | 可插拔加密层 | Blowfish | TLS |
| 认证 | 脚本层实现 | rndUUID | JWT + Challenge |
| 反 DDoS | Challenge 提高难度 | 无 | Challenge + IP 限流 |
