// theseed docs theme — 默认主题 + 品牌配色（custom.css 见同目录）
// VitePress 会自动发现本文件，无需在 config.mts 里引用。
import DefaultTheme from 'vitepress/theme'
import { defineComponent, h, onMounted, onUnmounted } from 'vue'
import './custom.css'

// a11y：`table { display: block }` 的横向滚动方案会把表格从可访问性树里
// 抹掉（屏幕阅读器失去行列语义）。改为渲染后把每张文档表包进滚动容器，
// 保留 <table> 语义，同时保留横向滚动能力（审计发现 custom.css:72）。
function wrapTables(root: ParentNode): void {
  const tables = root.querySelectorAll('.vp-doc table')
  tables.forEach((table) => {
    const parent = table.parentElement
    if (!parent || parent.classList.contains('table-scroll')) return
    const wrap = document.createElement('div')
    wrap.className = 'table-scroll'
    parent.insertBefore(wrap, table)
    wrap.appendChild(table)
  })
}

const ThemedLayout = defineComponent({
  name: 'ThemedLayout',
  setup() {
    let observer: MutationObserver | undefined
    onMounted(() => {
      wrapTables(document.body)
      // 站点内容按路由异步渲染：观察后续插入的表格一并包裹。
      observer = new MutationObserver(() => wrapTables(document.body))
      observer.observe(document.body, { childList: true, subtree: true })
    })
    onUnmounted(() => observer?.disconnect())
    return () => h(DefaultTheme.Layout)
  },
})

export default {
  extends: DefaultTheme,
  Layout: ThemedLayout,
}
