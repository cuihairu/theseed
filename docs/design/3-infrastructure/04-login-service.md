# Login Service — 登录服务与接入运维

> 来源：BigWorld `LoginApp + Login Challenge + statusCheck + clearIPAddressBans + controlled shutdown`。
> 这层不是单纯的 Gateway 附件，而是独立的登录与接入子系统。

---

## 0. 设计边界

```
Login Service 负责：
  - 登录请求生命周期
  - challenge 策略选择与校验
  - 登录失败与限流统计
  - ban / temporary block / challenge retry
  - 登录相关运维命令与状态检查

Login Service 不负责：
  - 游戏内 Entity 运行时逻辑
  - Base / Cell 主路径路由
  - 业务跨服消息
  - OTel metrics / traces 的存储后端
```

和 Gateway 的边界：

```
Gateway
  - 连接接入
  - TLS / 协议适配
  - 初步认证握手

Login Service
  - challenge / session / 风控 / ban / 状态检查
  - 登录运维命令与统计
```

---

## 1. 为什么要独立成篇

BigWorld 的登录面不只是“转发到 BaseApp 前做个 challenge”。

它实际已经包含：

```
1. challenge factory 配置
2. challenge 统计
3. 重复请求重发
4. 失败计数与风控
5. ban 清理命令
6. statusCheck
7. controlled shutdown
```

因此如果只在 Gateway 文档里写：

```
Challenge + TLS + 限流
```

会把登录服务的系统边界压扁成“接入实现细节”。

---

## 2. 职责拆分

### 2.1 接入职责

```
  - 接收 login 请求
  - 识别连接来源与协议
  - 建立临时登录会话
```

### 2.2 挑战职责

```
  - 选择 challenge type
  - 发放 challenge
  - 验证 challenge response
  - 处理重复提交 / 重发
  - 采样 challenge 耗时与失败率
```

### 2.3 风控职责

```
  - IP 级限流
  - temporary bans
  - repeated pending 请求抑制
  - challenge failure 统计
```

### 2.4 会话职责

```
  - 登录成功后创建会话
  - 绑定 token / account / uid
  - 向后续网关或会话层交接
```

---

## 3. 架构

```
Client
  → Gateway
  → Login Service
      ├─ Challenge Registry
      ├─ Rate Limit / Ban Store
      ├─ Login Request Tracker
      ├─ Session Issuer
      └─ Ops Hooks
```

组件职责：

```
Challenge Registry
  - 注册 challenge type
  - 运行时选择 challenge factory

Login Request Tracker
  - 跟踪 pending 请求
  - 支持 challenge 重发
  - 避免重复并发校验

Rate Limit / Ban Store
  - IP / account / device 维度限流
  - 临时封禁与过期清理
```

---

## 4. 核心接口

### 4.1 Challenge 抽象

```cpp
// login/ILoginChallenge.h

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

### 4.2 Challenge Registry

```cpp
// login/IChallengeRegistry.h

class IChallengeRegistry {
public:
    virtual ~IChallengeRegistry() = default;

    virtual void registerFactory(std::unique_ptr<ILoginChallengeFactory> f) = 0;
    virtual std::vector<std::string> supportedTypes() const = 0;
    virtual std::unique_ptr<ILoginChallenge> create(
        const std::string& type) const = 0;
};
```

### 4.3 登录服务接口

```cpp
// login/ILoginService.h

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

---

## 5. 请求状态机

```
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

要求：

```
1. 重复 login 请求不能无限生成 challenge
2. 已发 challenge 的请求要支持重发
3. challenge 验证失败要计入风控与统计
4. 状态转移必须可审计
```

---

## 6. 运维控制面

登录服务需要明确暴露一组受控运维能力：

### 6.1 Inspect

```
  - 当前 pending 登录数
  - challenge type 配置
  - successes / failures / rateLimited
  - challenge calculation / verification latency
  - ban store 摘要
```

### 6.2 Command

```
  - statusCheck
  - clearTemporaryBans
  - setChallengeType
  - setDraining
  - controlledShutdown
```

### 6.3 限制

```
  - 不允许在线修改协议字段
  - 不允许绕过审计直接清空全部会话
  - challenge type 切换必须校验目标实现存在
```

---

## 7. 数据与配置边界

### 7.1 配置项

```yaml
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

### 7.2 存储

```
可放 Redis / 内存 + 定时持久化的内容：
  - pending login state
  - temporary bans
  - rate limit counters

不放在登录服务长期保存的内容：
  - 账号权威资料
  - 角色持久化数据
  - 游戏内会话实体状态
```

---

## 8. 分阶段边界

```
MVP：
  - 单一 challenge type
  - 基础 pending 管理
  - IP 限流
  - statusCheck / clearTemporaryBans

Phase 2：
  - challenge factory registry
  - 更丰富的统计
  - 设备指纹 / account 维度风控

Phase 3：
  - 多 challenge policy
  - 与统一 Ops 平台联动
  - 更细粒度的动态风控编排
```

---

## 9. 与 BigWorld / KBEngine 的对比

| 维度 | BigWorld | KBEngine | theseed |
|------|---------|---------|---------|
| Login Challenge | 内建，多 factory | 基本无 | 独立 Login Service |
| 登录统计 | 有 | 较弱 | 显式 stats snapshot |
| ban 清理命令 | 有 | 较弱 | Ops 命令 |
| 状态检查 | 有 | 较弱 | statusCheck |
| controlled shutdown | 有 | 较弱 | 进入集群生命周期面 |
