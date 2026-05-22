# Server Merge — 合服工具链

> 合服是游戏运营的刚需，但 KBEngine 没有任何合服工具。
> BigWorld 有 consolidate_dbs / transfer_db / sync_db。
> theseed 需要完整的合服工具链：ID 重映射 + 冲突解决 + 关联修复。
> 但本篇只谈“多服业务数据合并”，不覆盖 SecondaryDB 搬运与归并。

---

## 0.5 引擎实现对照与取舍

### BigWorld 是怎么实现的

```
BigWorld 的合服不是单脚本搬库：
  - transfer_db / consolidate_dbs / sync_db 是不同工具
  - 合服前后往往还要做 repair 和关系修复
```

### KBEngine 是怎么实现的

```
KBEngine 基本没有完整的合服工具链。
如果要合服，通常需要自己补数据搬运和修复逻辑。
```

### 优缺点

```
BigWorld 的优点：
  - 数据合并责任分离
  - 更适合大规模运营动作

KBEngine 的优点：
  - 简单
  - 适合小规模或手工处理

共同缺点：
  - 合服永远是高风险操作
```

### theseed 的取舍

```
theseed 把合服和 SecondaryDB 搬运分开，
因为它们是两类不同的数据运维问题，
不能写成同一件事。
```

---

## 0. 范围边界

```
本篇负责：
  - 多 Realm / 多服业务数据合并
  - EntityID / 唯一索引 / 关联关系修复
  - 合并前分析、预览、执行、回滚

本篇不负责：
  - BaseApp 本地 SecondaryDB snapshot / transfer
  - generation consolidate
  - Schema 同步与修复平台
```

BigWorld 的几类工具在设计上应拆开：

| 工具 | 目标 | 是否属于本篇 |
|------|------|------|
| merge | 多服业务数据合并 | 是 |
| transfer_db | SecondaryDB 文件搬运 | 否 |
| consolidate_dbs | SecondaryDB 归并 | 否 |
| sync_db | Schema / 数据同步修复 | 否 |

相关边界见：

[03-local-archive-and-secondary-db](03-local-archive-and-secondary-db.md)

---

## 1. 冲突场景

```
服 A 和服 B 合并到服 C：

冲突 1：EntityID 重叠
  服 A: Player id=10042 (Alice)
  服 B: Player id=10042 (Bob)

冲突 2：唯一索引冲突
  服 A: Player name="Alice"
  服 B: Player name="Alice"（不同的人）

冲突 3：关联数据断裂
  Mail.sender 还指向旧 ID

冲突 4：自增 ID 重叠

冲突 5：EntityCall 引用失效
```

---

## 2. 合服流程

```
theseed-merge 工具：

Phase 1: 分析
  ├─ 读取两个服的 def 文件（确保 schema 兼容）
  ├─ 扫描两个服的数据库
  ├─ 检测冲突：ID 范围重叠、唯一索引冲突、关联关系图
  └─ 生成冲突报告

Phase 2: ID 重映射
  ├─ 偏移映射：服 B 的 ID + offset
  ├─ 命名空间映射：id → realm_a:{id}
  └─ 完全重分配

Phase 3: 唯一索引冲突解决
  ├─ name → 自动加后缀（"Alice" → "Alice_B"）
  ├─ account → 合并账号（人工确认）
  └─ 自定义字段 → 脚本回调

Phase 4: 关联数据修复
  ├─ Mail.sender/receiver → 新 ID
  ├─ Guild.leader/members → 批量替换
  ├─ EntityCall 引用 → 扫描 BLOB/JSON 替换
  └─ SUBTABLE 外键更新

Phase 5: 数据合并
  ├─ 按依赖顺序合并
  ├─ 验证外键完整性
  └─ 重建索引

Phase 6: 验证
  ├─ 数据完整性检查
  ├─ 唯一索引验证
  ├─ 外键一致性验证
  └─ 生成合并报告
```

---

## 3. 合服配置

```xml
<!-- config/merge.xml -->
<merge xmlns="https://theseed.dev/schema/merge">

    <sourceRealms>
        <realm name="realm_a">
            <database host="mysql-a.internal" port="3306" name="theseed_game"/>
        </realm>
        <realm name="realm_b">
            <database host="mysql-b.internal" port="3306" name="theseed_game"/>
        </realm>
    </sourceRealms>

    <targetRealm name="realm_merged">
        <database host="mysql-merged.internal" port="3306" name="theseed_game"/>
    </targetRealm>

    <idRemap strategy="offset">
        <offset realm="realm_a" value="0"/>
        <offset realm="realm_b" value="auto"/>
    </idRemap>

    <conflictResolution default="suffix">
        <field name="playerName" strategy="suffix" suffixTemplate="_{realm}"/>
        <field name="account" strategy="script" script="merge_account_conflict"/>
    </conflictResolution>

    <relationFix autoScanJson="true" autoScanBlob="true">
        <explicitRelation table="tbl_Mail" fields="sender_id,receiver_id"/>
        <explicitRelation table="tbl_Guild" fields="leader_id"/>
    </relationFix>

</merge>
```

---

## 4. CLI 工具

```bash
# 分析冲突
theseed-merge analyze --config config/merge.xml --output report.json

# 预览（dry run）
theseed-merge preview --config config/merge.xml

# 执行合并
theseed-merge execute --config config/merge.xml --output merge_result.json

# 回滚
theseed-merge rollback --config config/merge.xml --from merge_result.json
```

---

## 5. 与其他数据运维能力的关系

合服不是数据运维面的全部。

theseed 需要显式拆开四条线：

```
1. 主持久化
   - load / save / remove / query

2. 合服
   - merge analyze / preview / execute / rollback

3. 本地归档暂存
   - snapshot / transfer / consolidate / cleanup

4. 同步修复
   - schema diff / data repair / consistency check
```

如果把这四类能力混成一个“全能工具”，后期边界会非常混乱。
