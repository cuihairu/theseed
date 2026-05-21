---
layout: home

hero:
  name: theseed
  text: 游戏服务器引擎
  tagline: 继承 BigWorld / KBEngine 架构思想，面向千台集群的现代游戏服务器引擎
  actions:
    - theme: brand
      text: 设计文档
      link: /design/

features:
  - title: 1-core 核心运行时
    details: Tick 模型、Entity Base/Cell 双体、AOI 十字链表、Ghost/Witness、EntityCall、属性同步、迁移、定时器、容错
  - title: 2-data 数据层
    details: XML + XSD 数据定义、整存/展开双策略、JSON 原生支持、多后端、合服工具
  - title: 3-infrastructure 基础设施
    details: Gateway 网关、Aeron + NATS 消息总线、Redis、异步框架、统一配置
  - title: 4-gameplay 玩法支撑
    details: 服务端物理/导航、控制器系统、Space/Scene、内置实体、生命周期钩子、脚本绑定
  - title: 5-scripting 脚本层
    details: Python/Lua 安全防护、客户端三防线、L1-L4 分级热更新、滚动部署
  - title: 6-client 客户端 SDK
    details: 一份 XML 定义 → 自动生成 Unity/UE5/Cocos 客户端代码，属性插值、断线重连
  - title: 7-observability 可观测性
    details: OTel 全链路追踪、Metrics、结构化日志、DAP Debug、Profiling、告警
---
