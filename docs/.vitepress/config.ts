import { defineConfig } from 'vitepress'

export default defineConfig({
  lang: 'zh-CN',
  title: 'theseed',
  description: '对标 BigWorld / KBEngine 服务端系统面的现代游戏服务器引擎设计文档',
  base: '/theseed/',

  markdown: {
    html: false,
  },

  search: {
    provider: 'local',
  },

  themeConfig: {
    nav: [{ text: '设计文档', link: '/design/' }],

    sidebar: {
      '/design/': [
        {
          text: '0-foundation 设计基线',
          collapsed: false,
          items: [
            { text: '01 MVP 架构基线', link: '/design/0-foundation/01-mvp-architecture-baseline' },
            { text: '02 审计口径与分层', link: '/design/0-foundation/02-audit-scope-and-layering' },
          ],
        },
        {
          text: '1-runtime-model 运行时模型',
          collapsed: false,
          items: [
            { text: '01 Tick 模型', link: '/design/1-runtime-model/01-tick-model' },
            { text: '02 Entity 系统', link: '/design/1-runtime-model/02-entity-system' },
            { text: '03 Timer 与 Memory', link: '/design/1-runtime-model/03-timer-and-memory' },
          ],
        },
        {
          text: '2-replication-and-space 复制与空间',
          collapsed: false,
          items: [
            { text: '01 Space Topology 与 AOI', link: '/design/2-replication-and-space/01-space-topology-and-aoi' },
            { text: '02 Ghost 与 Witness', link: '/design/2-replication-and-space/02-ghost-and-witness' },
            { text: '03 Runtime Communication 与 Transport', link: '/design/2-replication-and-space/03-runtime-communication-and-transport' },
            { text: '04 Property Replication', link: '/design/2-replication-and-space/04-property-replication' },
            { text: '05 Entity Migration', link: '/design/2-replication-and-space/05-entity-migration' },
            { text: '06 AoI Update 与 Load Bounds', link: '/design/2-replication-and-space/06-aoi-update-and-load-bounds' },
            { text: '07 BSP Rebalance 与 Offload', link: '/design/2-replication-and-space/07-bsp-rebalance-and-offload' },
          ],
        },
        {
          text: '3-cluster-and-availability 集群与可用性',
          collapsed: false,
          items: [
            { text: '01 Fault Tolerance', link: '/design/3-cluster-and-availability/01-fault-tolerance' },
            { text: '02 Backup Hash 与 HA', link: '/design/3-cluster-and-availability/02-backup-hash-and-ha' },
            { text: '03 Cluster Lifecycle', link: '/design/3-cluster-and-availability/03-cluster-lifecycle' },
            { text: '04 Runtime Profiler 与 Load Feedback', link: '/design/3-cluster-and-availability/04-runtime-profiler-and-load-feedback' },
          ],
        },
        {
          text: '4-data-and-ops 数据与运维',
          collapsed: false,
          items: [
            { text: '01 数据定义', link: '/design/4-data-and-ops/01-data-definition' },
            { text: '02 Persistence', link: '/design/4-data-and-ops/02-persistence' },
            { text: '03 Local Archive 与 SecondaryDB', link: '/design/4-data-and-ops/03-local-archive-and-secondary-db' },
            { text: '04 Data Ops Toolchain', link: '/design/4-data-and-ops/04-data-ops-toolchain' },
            { text: '05 Server Merge', link: '/design/4-data-and-ops/05-server-merge' },
          ],
        },
        {
          text: '5-access-and-control-plane 接入与控制面',
          collapsed: false,
          items: [
            { text: '01 Gateway 与 Login', link: '/design/5-access-and-control-plane/01-gateway-and-login' },
            { text: '02 MessageBus 与 Cross-Realm', link: '/design/5-access-and-control-plane/02-message-bus-and-cross-realm' },
            { text: '03 Redis / Async / Config', link: '/design/5-access-and-control-plane/03-redis-async-and-config' },
            { text: '04 Ops Control Plane', link: '/design/5-access-and-control-plane/04-ops-control-plane' },
            { text: '05 Telemetry 与 Debug', link: '/design/5-access-and-control-plane/05-telemetry-and-debug' },
          ],
        },
        {
          text: '6-world-and-game-framework 世界与游戏框架',
          collapsed: false,
          items: [
            { text: '01 Physics', link: '/design/6-world-and-game-framework/01-physics' },
            { text: '02 Navigation', link: '/design/6-world-and-game-framework/02-navigation' },
            { text: '03 Controllers', link: '/design/6-world-and-game-framework/03-controllers' },
            { text: '04 World Streaming 与 Compiled Space', link: '/design/6-world-and-game-framework/04-world-streaming-and-compiled-space' },
            { text: '05 Built-in Entities', link: '/design/6-world-and-game-framework/05-built-in-entities' },
            { text: '06 Lifecycle 与 Script Binding', link: '/design/6-world-and-game-framework/06-lifecycle-and-script-binding' },
          ],
        },
        {
          text: '7-scripting-and-client 脚本与客户端',
          collapsed: false,
          items: [
            { text: '01 Script Security', link: '/design/7-scripting-and-client/01-script-security' },
            { text: '02 Hot Update', link: '/design/7-scripting-and-client/02-hot-update' },
            { text: '03 Script Debug', link: '/design/7-scripting-and-client/03-script-debug' },
            { text: '04 Client SDK', link: '/design/7-scripting-and-client/04-client-sdk' },
          ],
        },
        {
          text: '8-reference 参考',
          collapsed: false,
          items: [{ text: '来源追溯', link: '/design/8-reference/source-attribution' }],
        },
      ],
    },

    socialLinks: [{ icon: 'github', link: 'https://github.com/cuihairu/theseed' }],
  },
})
