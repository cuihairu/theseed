---
layout: home

hero:
  name: theseed
  text: 游戏服务器引擎
  tagline: 以 BigWorld 为源头、KBEngine 为参考实现之一，面向现代 MMO 的分布式游戏服务器引擎
  actions:
    - theme: brand
      text: 设计文档
      link: /design/

features:
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><rect x="3.5" y="3.5" width="17" height="17" rx="2"/><path d="M3.5 9h17"/><path d="M9 9v11.5"/></svg>'
    title: 0-foundation 设计基线
    details: 引擎定位、MVP 范围、审计口径、分层原则、运行时硬边界
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><circle cx="12" cy="12" r="8.5"/><path d="M12 7v5.2l3.4 2"/></svg>'
    title: 1-runtime-model 运行时模型
    details: Tick 模型、Entity 双体、Timer、对象池、运行时内存
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><rect x="3.5" y="3.5" width="9.5" height="9.5" rx="1.5"/><rect x="11" y="11" width="9.5" height="9.5" rx="1.5"/><path d="M13 6.5h4"/><path d="M6.5 13v4"/></svg>'
    title: 2-replication-and-space 复制与空间
    details: AOI、Ghost/Witness、EntityCall、迁移、属性复制、BSP
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><circle cx="12" cy="5.5" r="2.6"/><circle cx="5.5" cy="17.5" r="2.6"/><circle cx="18.5" cy="17.5" r="2.6"/><path d="M12 8.1v3.4"/><path d="m7.6 15.5 2.9-2.7"/><path d="m16.4 15.5-2.9-2.7"/><circle cx="12" cy="12.5" r="1.2"/></svg>'
    title: 3-cluster-and-availability 集群与可用性
    details: 容错、BackupHash、停服/退役、负载反馈闭环
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><ellipse cx="12" cy="6" rx="7.5" ry="2.8"/><path d="M4.5 6v12c0 1.55 3.36 2.8 7.5 2.8s7.5-1.25 7.5-2.8V6"/><path d="M4.5 12c0 1.55 3.36 2.8 7.5 2.8s7.5-1.25 7.5-2.8"/></svg>'
    title: 4-data-and-ops 数据与运维
    details: 数据定义、持久化、SecondaryDB、本地归档、数据工具链
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><path d="M14 3.5h4.5a2 2 0 0 1 2 2v13a2 2 0 0 1-2 2H14"/><path d="m10 7.5 4 4.5-4 4.5"/><path d="M14 12H3.5"/></svg>'
    title: 5-access-and-control-plane 接入与控制面
    details: Gateway/Login、MessageBus、Ops Control Plane、Telemetry
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><circle cx="12" cy="12" r="8.5"/><path d="M3.5 12h17"/><path d="M12 3.5c2.4 2.3 3.7 5.3 3.7 8.5s-1.3 6.2-3.7 8.5"/><path d="M12 3.5c-2.4 2.3-3.7 5.3-3.7 8.5s1.3 6.2 3.7 8.5"/></svg>'
    title: 6-world-and-game-framework 世界与游戏框架
    details: 物理、导航、控制器、World Streaming、生命周期、脚本绑定
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><rect x="3" y="4.5" width="18" height="15" rx="2"/><path d="m7.5 10 2.6 2.6L7.5 15.2"/><path d="M13 15.2h4"/></svg>'
    title: 7-scripting-and-client 脚本与客户端
    details: 脚本安全、热更新、脚本调试、客户端 SDK、代码生成
---
