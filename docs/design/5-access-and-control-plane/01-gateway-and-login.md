# Gateway & Login — 接入层、登录服务与 Challenge 运维边界

> 来源源头：BigWorld `LoginApp / Login Challenge / statusCheck / clearIPAddressBans`。
> 参考实现：KBEngine `LoginApp` 的轻量接入路径。
> theseed 在此基础上把 Gateway 与 Login Service 的职责拆清。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 的 LoginApp 不只是接入点：
  - challenge
  - pending request
  - IP ban 管理
  - status check
  - 受控停服配合

它本质上是一个独立运维子系统。
```

### KBEngine 是怎么实现的

```
KBEngine 也有 LoginApp，
但整体更偏向接入与会话分发，
运维控制面没有 BigWorld 那么厚。
```

### 实现能力对比

| 维度 | BigWorld | KBEngine | theseed 取舍 |
|------|----------|----------|--------------|
| LoginApp 定位 | 登录入口 + 登录状态机 + 运维子系统 | 轻量登录入口 + 会话分发 | 当前参考 BigWorld，先实现登录面核心服务 |
| Gateway 边界 | 接入和登录面职责存在历史耦合 | LoginApp 承担较多接入职责 | 逻辑上保留 Gateway/Login 边界，MVP 不强制拆进程 |
| Challenge | Login Challenge / factory 能力明确 | 基本不强调复杂 challenge | 当前先保留开关与接口位置，复杂状态机后续增强 |
| Pending request | 登录请求状态可跟踪、可控 | 相对轻量 | MVP 可先省略复杂状态机，但接口预留审计点 |
| IP ban / 风控 | 登录面内建临时 ban、清理命令 | 较弱 | Phase C 引入，避免 MVP 过重 |
| statusCheck | 登录面运维命令的一部分 | 较弱 | Phase C 接入统一 Ops Control Plane |
| 受控停服 | 与登录入口配合 draining / shutdown | 较弱 | Phase C 做 controlledShutdown / draining |
| 区服选择 | 不是核心抽象 | 常见实现依赖 LoginApp 路由 | 当前由 LoginApp 直接返回分区列表和角色摘要 |
| 分区入口路由 | 依赖集群状态或服务发现 | 轻量分发 | 当前先支持每区固定 ip:port，策略化放入待定计划 |
| 会话签发 | 登录服务完成 session handoff | LoginApp 直接分发会话信息 | theseed 用 ISessionIssuer 签发绑定 accountId + realmId 的 token |
| 部署形态 | 偏完整服务面 | 更容易单点落地 | 当前先单 LoginApp 落地，多入口和 Pipeline 放入待定计划 |

### 优缺点

```
BigWorld 的优点：
  - 登录链路与运维链路一体化
  - 攻防和停服边界更完整

KBEngine 的优点：
  - 简单
  - 更容易先落地

共同缺点：
  - 如果边界不清，Gateway 和 Login 很容易混层
```

### theseed 的取舍

```
theseed 选择把 Gateway 和 Login Service 分开写，
并把 Challenge 运维边界显式立住，
避免把登录服务缩成“一个接入端口”。
```

### 为什么不能只剩一个 Gateway

```
如果把 Gateway 和 Login 合成“一个接入服务”，
很快就会出现三种混乱：
  1. 连接接入和 challenge 状态机绑死
  2. TLS / 协议适配和风控 / ban / pending request 混在一起
  3. 登录面运维能力退化成“查连接数”
```

---

## 0. 设计边界

本篇负责：

```
  - 外部接入链路
  - Gateway 与 Login Service 的职责拆分
  - challenge / pending / ban / session handoff
  - 登录面运维边界
```

本篇不负责：

```
  - 游戏内 Entity runtime
  - MessageBus 控制面
  - Watcher 式统一控制入口
  - OTel 后端与可视化平台
```

这些主题分别见：

```
02-message-bus-and-cross-realm
04-ops-control-plane
05-telemetry-and-debug
../3-cluster-and-availability/03-cluster-lifecycle
```

---

## 1. 为什么要把 Gateway 和 Login 放在同一层

旧结构把 `Gateway` 和 `Login Service` 分成两篇没有问题，
但放在旧目录里时容易让人误判：

```
Gateway = 外部接入
Login Service = 某个功能子页
```

实际更准确的理解是：

```
Gateway + Login Service
  一起构成 Access Plane
```

其中：

```
Gateway
  负责连接接入、协议适配、TLS、粗粒度限流

Login Service
  负责 challenge、pending request、风控、ban、statusCheck、登录会话交接
```

这也是 BigWorld `LoginApp` 真正体现出来的系统边界。

所以更准确的归类是：

```
Gateway
  = 接入代理层

Login Service
  = 登录状态机 + 风控 + 会话签发层
```

---

## 2. 总体架构

```text
Client
  ├─ TCP / WebSocket / HTTP
  ▼
Gateway
  ├─ TLS termination
  ├─ protocol adapter
  ├─ coarse rate limit
  └─ route to login
  ▼
Login Service
  ├─ challenge registry
  ├─ request tracker
  ├─ rate-limit / ban store
  ├─ session issuer
  └─ ops hooks
  ▼
Session / Router handoff
  ▼
Base / Game entry
```

---

## 3. Gateway 的职责

Gateway 负责：

```
  - 外部端口监听
  - TLS 终结
  - 协议解包与连接管理
  - 粗粒度 IP / 连接限流
  - 把登录流量转交给 Login Service
```

Gateway 不负责：

```
  - challenge 生命周期管理
  - pending login 去重
  - ban 清理策略
  - 登录统计面板
```

### 3.1 接口

```cpp
class IGateway {
public:
    virtual void start(const GatewayConfig& config) = 0;
    virtual void stop() = 0;
    virtual size_t activeConnections() const = 0;
    virtual void kickConnection(ConnectionId id, const std::string& reason) = 0;
    virtual GatewayStats getStats() const = 0;
};
```

---

## 4. Login Service 的职责

Login Service 负责：

```
  - 生成与校验 challenge
  - 维护 pending login request
  - 统计 challenge success / failure / retry
  - 临时 ban / rate limit / request dedupe
  - 登录成功后的 session issuance
  - statusCheck / clearTemporaryBans / draining
```

### 4.1 核心接口

```cpp
class ILoginService {
public:
    virtual ~ILoginService() = default;

    virtual Future<LoginReply> beginLogin(const LoginRequest& request) = 0;
    virtual Future<LoginReply> verifyChallenge(
        const ChallengeVerificationRequest& request) = 0;

    virtual Future<void> clearTemporaryBans() = 0;
    virtual LoginStatsSnapshot stats() const = 0;
};
```

### 4.2 Challenge 抽象

```cpp
class ILoginChallenge {
public:
    virtual ~ILoginChallenge() = default;

    virtual std::string type() const = 0;
    virtual Challenge generate(const LoginRequestMeta& meta) = 0;
    virtual VerifyResult verify(const Challenge& challenge,
                                const ChallengeResponse& response,
                                const LoginRequestMeta& meta) = 0;
};
```

---

## 5. 登录请求状态机

```text
Idle
  → Pending
  → ChallengeIssued
  → ChallengeVerified
  → SessionIssued
  → Complete

失败路径：
  Pending → Rejected
  ChallengeIssued → ChallengeFailed
  ChallengeIssued → Expired
```

关键约束：

```
1. 重复 login 请求不能无限发 challenge
2. challenge 重发必须在原 request 上继续
3. challenge 失败要进入统计与风控
4. session issuance 和 request 完成要可审计
```

---

## 6. 运维边界

BigWorld 的登录面不是“登录代码里顺带有几个命令”，
而是一个小型运维子系统。

theseed 文档现在明确把它拆成三类能力：

### 6.1 Inspect

```
  - pending login count
  - challenge type
  - successes / failures / rate limited
  - ban store 摘要
  - 当前 draining 状态
```

### 6.2 Command

```
  - statusCheck
  - clearTemporaryBans
  - setChallengeType
  - setDraining
  - controlledShutdown
```

### 6.3 Guardrail

```
  - 不允许在线改协议字段
  - 不允许绕过审计直接清空会话权威
  - challenge policy 切换必须校验实现已注册
```

其中统一命令入口和权限模型，不在本篇展开，见：

`04-ops-control-plane`

---

## 7. Session Handoff 与路由

登录成功后，Access Plane 只负责完成交接，不接管游戏内权威。

```text
Login Service
  → issue session / token
  → choose entry route
  → hand off to gateway router / game gateway
  → runtime authority moved to Base / game session side
```

这点要和 KBEngine / BigWorld 的老式 LoginApp 思路保持一致：

```
登录服务不是游戏逻辑宿主。
```

---

## 8. 配置边界

```yaml
gateway:
  listeners:
    - protocol: tcp
      port: 20000
    - protocol: websocket
      port: 20001

  tls:
    cert_file: /etc/theseed/certs/server.pem
    key_file: /etc/theseed/certs/server.key

login_service:
  challenge:
    default_type: hash_pow
    timeout_ms: 10000
    max_retries: 2

  rate_limit:
    per_ip_per_minute: 30
    pending_per_ip: 4

  bans:
    temporary_ttl_sec: 1800
```

---

## 9. 分阶段边界

```text
MVP：
  - 单一 challenge type
  - 基础 pending 管理
  - IP 限流
  - statusCheck / clearTemporaryBans

Phase 2：
  - challenge factory registry
  - 更丰富的统计
  - account / device 维度风控

Phase 3：
  - 多 challenge policy
  - 与统一 Ops 平台联动
  - 更细粒度的接入编排
```

---

## 10. 与 BigWorld / KBEngine / theseed 的对比

| 维度 | BigWorld | KBEngine | theseed |
|------|------|------|------|
| Login Challenge | 内建，多 factory | 基本无 | 独立 Login Service |
| statusCheck | 有 | 较弱 | 明确保留 |
| ban 清理命令 | 有 | 较弱 | 明确保留 |
| Gateway | 简化接入面 | 简化 TCP 转发 | 独立 Gateway |
| 登录运维面 | 有 | 较弱 | 设计边界已立住 |

---

## 11. 一句话判断

本篇的重点不是”登录如何握手”，而是：

```
theseed 已经把 BigWorld 风格的登录服务系统边界
从单纯接入细节提升为 Access Plane 的独立能力面。
```

---

## 12. LoginApp — 参考 BigWorld 的实现基线

> 本节先回到 BigWorld 的实现方式，作为当前阶段的落地基线。
> LoginApp 先按登录面核心服务实现，包含认证、分区列表、角色摘要、风控、session handoff，
> 不先引入可组装 Pipeline 的抽象化改造。

### 12.1 LoginApp 的职责

LoginApp 参考 BigWorld 的登录面实现，职责包括：

```text
  - 认证
  - 分区列表查询
  - 角色摘要返回
  - pending request 维护
  - 临时 ban / 风控
  - statusCheck
  - session handoff
```

LoginApp 不承担游戏内 Entity runtime，不承担消息总线控制面，也不承担统一运维平台。

### 12.2 登录流程

```text
Client
  → login(account, credentials)
  → LoginApp 校验账号
  → 返回分区列表 + 每区角色摘要 + 分区 ip:port

Client 选择分区
  → connect realm.ip:realm.port
  → loginGateway(account, token, realmId)
  → 分区验证 session
  → 创建 Account Entity
  → reqAvatarList / selectAvatarGame
```

单区模式时可跳过分区选择，但默认实现仍按分区入口处理。

### 12.3 BigWorld 对照的实现点

```text
BigWorld 风格的关键点：
  - LoginApp 是一个独立登录面子系统
  - 登录、风控、ban、statusCheck、session handoff 在同一边界内
  - 分区信息与角色摘要由登录面返回
  - 客户端最终直连分区入口
```

### 12.4 当前实现基线

```cpp
struct RealmInfo {
    std::string realmId;
    std::string name;
    std::string status;
    std::string host;
    uint16_t port;
    std::vector<CharacterSummary> characters;
};

class ILoginApp {
public:
    virtual ~ILoginApp() = default;
    virtual LoginReply login(const LoginRequest& request) = 0;
    virtual RealmList queryRealms(const AccountId& accountId) = 0;
    virtual LoginReply selectRealm(const AccountId& accountId,
                                   const std::string& realmId) = 0;
    virtual void clearTemporaryBans() = 0;
    virtual LoginStatsSnapshot stats() const = 0;
};
```

### 12.5 配置基线

```yaml
login_app:
  endpoint: login.example.com
  auth:
    type: password
  realm_source:
    type: static_list
  realms:
    - id: "realm_east"
      name: "华东一区"
      host: "10.0.1.1"
      port: 20000
    - id: "realm_south"
      name: "华南一区"
      host: "10.0.2.1"
      port: 20000
  bans:
    temporary_ttl_sec: 1800
  challenge:
    enabled: false
```

### 12.6 讨论待确定的计划

以下内容先保留为讨论，不进入当前实现基线：

```text
  - 可组装 Login Pipeline
  - 独立 Gateway 进程
  - login endpoint 发现 / DNS 轮询 / LB
  - 分区入口策略化（Static / Registry / Fixed）
  - Challenge 状态机增强
  - 更细粒度的风控策略
```

### 12.7 设计文档与实现基线的关系

本文档第 1-11 章描述的是 theseed Access Plane 的完整系统边界。
第 12 章当前先采用 BigWorld 式实现基线，后续再根据验证结果决定是否引入更抽象的管道化方案。
