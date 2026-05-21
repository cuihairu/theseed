import { defineConfig } from 'vitepress'

export default defineConfig({
  lang: 'zh-CN',
  title: 'theseed',
  description: '继承 BigWorld / KBEngine 架构思想，面向千台集群的现代游戏服务器引擎',
  base: '/theseed/',

  markdown: {
    html: false,
  },

  search: {
    provider: 'local',
  },

  themeConfig: {
    nav: [
      { text: '设计文档', link: '/design/' },
    ],

    sidebar: {
      '/design/': [
        {
          text: '0-foundation 设计基线',
          collapsed: false,
          items: [
            { text: '01 MVP 架构总纲', link: '/design/0-foundation/01-mvp-architecture-baseline' },
          ],
        },
        {
          text: '1-core 核心运行时',
          collapsed: false,
          items: [
            { text: '01 Tick 模型', link: '/design/1-core/01-tick-model' },
            { text: '02 Entity 系统', link: '/design/1-core/02-entity-system' },
            { text: '03 AOI 与 Space', link: '/design/1-core/03-aoi-and-space' },
            { text: '04 Ghost 与 Witness', link: '/design/1-core/04-ghost-and-witness' },
            { text: '05 通信', link: '/design/1-core/05-communication' },
            { text: '06 属性同步', link: '/design/1-core/06-property-sync' },
            { text: '07 Entity 迁移', link: '/design/1-core/07-entity-migration' },
            { text: '08 定时器与对象池', link: '/design/1-core/08-timer-and-pool' },
            { text: '09 容错', link: '/design/1-core/09-fault-tolerance' },
            { text: '10 Backup Hash 与 HA', link: '/design/1-core/10-backup-hash-and-ha' },
            { text: '11 AoI Update 与 Load Bounds', link: '/design/1-core/11-aoi-update-schemes-and-load-bounds' },
            { text: '12 BSP Rebalance 与 Offload', link: '/design/1-core/12-bsp-rebalance-and-offload' },
          ],
        },
        {
          text: '2-data 数据层',
          collapsed: false,
          items: [
            { text: '01 数据定义', link: '/design/2-data/01-data-definition' },
            { text: '02 持久化', link: '/design/2-data/02-persistence' },
            { text: '03 合服', link: '/design/2-data/03-server-merge' },
            { text: '04 SecondaryDB / 本地归档暂存', link: '/design/2-data/04-secondary-db' },
            { text: '05 Data Ops Toolchain', link: '/design/2-data/05-data-ops-toolchain' },
          ],
        },
        {
          text: '3-infrastructure 基础设施',
          collapsed: false,
          items: [
            { text: '01 Gateway', link: '/design/3-infrastructure/01-gateway' },
            { text: '02 消息总线', link: '/design/3-infrastructure/02-message-bus' },
            { text: '03 Redis / 异步 / 配置', link: '/design/3-infrastructure/03-redis-async-config' },
            { text: '04 Login Service', link: '/design/3-infrastructure/04-login-service' },
            { text: '05 集群生命周期', link: '/design/3-infrastructure/05-cluster-lifecycle' },
            { text: '06 Runtime Profiler 与 Load Feedback', link: '/design/3-infrastructure/06-runtime-profiler-and-load-feedback' },
            { text: '07 Runtime Transport Reliability', link: '/design/3-infrastructure/07-runtime-transport-reliability' },
          ],
        },
        {
          text: '4-gameplay 玩法支撑',
          collapsed: false,
          items: [
            { text: '01 物理', link: '/design/4-gameplay/01-physics' },
            { text: '02 导航', link: '/design/4-gameplay/02-navigation' },
            { text: '03 控制器', link: '/design/4-gameplay/03-controllers' },
            { text: '04 Space / Scene', link: '/design/4-gameplay/04-space-scene' },
            { text: '05 内置实体', link: '/design/4-gameplay/05-built-in-entities' },
            { text: '06 生命周期与脚本绑定', link: '/design/4-gameplay/06-lifecycle' },
            { text: '07 World Streaming / Compiled Space', link: '/design/4-gameplay/07-world-streaming-and-compiled-space' },
          ],
        },
        {
          text: '5-scripting 脚本层',
          collapsed: false,
          items: [
            { text: '01 安全', link: '/design/5-scripting/01-security' },
            { text: '02 热更新', link: '/design/5-scripting/02-hot-update' },
          ],
        },
        {
          text: '6-client 客户端 SDK',
          collapsed: false,
          items: [
            { text: '01 SDK 架构', link: '/design/6-client/01-sdk-architecture' },
          ],
        },
        {
          text: '7-observability 可观测性',
          collapsed: false,
          items: [
            { text: '01 OTel 集成', link: '/design/7-observability/01-otel-integration' },
            { text: '02 运维控制面', link: '/design/7-observability/02-ops-control-plane' },
          ],
        },
        {
          text: '8-reference 参考',
          collapsed: false,
          items: [
            { text: '来源追溯', link: '/design/8-reference/source-attribution' },
          ],
        },
      ],
    },

    socialLinks: [
      { icon: 'github', link: 'https://github.com/cuihairu/theseed' },
    ],
  },
})
