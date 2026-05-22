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
  - title: 0-foundation 设计基线
    details: MVP 范围、审计口径、分层原则、运行时硬边界
  - title: 1-runtime-model 运行时模型
    details: Tick 模型、Entity 双体、Timer、对象池、运行时内存
  - title: 2-replication-and-space 复制与空间
    details: AOI、Ghost/Witness、EntityCall、迁移、属性复制、BSP
  - title: 3-cluster-and-availability 集群与可用性
    details: 容错、BackupHash、停服/退役、负载反馈闭环
  - title: 4-data-and-ops 数据与运维
    details: 数据定义、持久化、SecondaryDB、本地归档、数据工具链
  - title: 5-access-and-control-plane 接入与控制面
    details: Gateway/Login、MessageBus、Ops Control Plane、Telemetry
  - title: 6-world-and-game-framework 世界与游戏框架
    details: 物理、导航、控制器、World Streaming、生命周期、脚本绑定
  - title: 7-scripting-and-client 脚本与客户端
    details: 脚本安全、热更新、脚本调试、客户端 SDK、代码生成
---
