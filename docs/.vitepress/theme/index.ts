// theseed docs theme — 默认主题 + 品牌配色（custom.css 见同目录）
// VitePress 会自动发现本文件，无需在 config.mts 里引用。
import DefaultTheme from 'vitepress/theme'
import './custom.css'

// 表格横向滚动与可访问性（display:table 语义）改由 SSG 期 transformHtml 把
// <table> 包进 .table-scroll 容器完成（见 config.ts），无需客户端 JS 搬移
// DOM——避免 MutationObserver 全页扫描开销与 Vue 管理的 <table> 被挪入
// 自建 div 引发的补丁/路由不一致（审计发现 index.ts:17/29）。
export default DefaultTheme
