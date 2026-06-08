# 应急 Runbook：statfs 高 QPS 打爆 MySQL 元数据库的缓解

## 适用场景

- SQL（MySQL）元数据引擎。
- 大量挂载点（本案例 ~12 万）持续执行 `statfs`，把元数据库打到接近熔断。
- **约束**：无法更新 JuiceFS 二进制、无法修改挂载参数，只能在 MySQL/DB 接入层动手。

## 根因

JuiceFS 的 `statfs` 在 SQL 引擎热路径（`pkg/meta/base.go` 的 `statRootFs`）默认会向 DB 读取两个计数器：

```sql
-- getCounter(usedSpace) / getCounter(totalInodes)，autocommit、无事务、无 FOR UPDATE
SELECT `name`,`value` FROM jfs_counter WHERE `name`=? LIMIT ?;
```

- `--fast-statfs` 默认为 `false`，因此**每次 statfs 都真打 DB**：`挂载数 × 1/s × 2 ≈ 24~25 万 QPS`，全部命中 `jfs_counter` 表里 `usedSpace`/`totalInodes` 两行。
- 这两行同时被**每个元数据写操作**做 `UPDATE jfs_counter SET value=value+? WHERE name=...`，是 InnoDB 超级热行。海量读 + 持续写既烧 CPU，又与正常元数据请求抢行锁/版本链。
- 关键：`statfs` 容忍陈旧值——客户端进程内每个心跳周期（默认 12s）已在后台刷新内存里的用量；且 `getCounter` 超时/报错时 `statRootFs` 会**静默回退**到内存值并仍返回成功。所以这部分流量是**纯冗余**，可以安全地在接入层缓存掉。

> 表名说明：默认前缀为 `jfs_`，即 `jfs_counter`。若部署时设置了自定义表前缀，表名为 `jfs_<prefix>_counter`，下文 SQL 请相应替换。

## 为什么不能只靠 mysqld 内部解决

- MySQL 8.0 没有结果缓存。
- MySQL 5.7 的 query cache 在"热行被频繁写"场景下会因写失效 + 全局互斥反而更糟。
- 无法用权限/触发器/视图在 mysqld 内部"只让 statfs 的 SELECT 变便宜"而不影响同表的 `UPDATE`（同表同行，无法按查询区分）。

要真正把 25 万 QPS 合并掉，必须在 **DB 接入层**放一个带 TTL 的结果缓存，对挂载点透明（连接的仍是原来的 MySQL 端点，不改二进制、不改挂载参数）。

---

## 主方案：ProxySQL TTL 结果缓存（推荐）

部署位置：把 ProxySQL 放在 DB 主机（后端指向本机 MySQL），并将挂载点已经在用的 MySQL 域名/VIP 指向 ProxySQL。挂载点无感知。

### 1. 配置后端与监控账号（按现有 MySQL 信息替换）

```sql
-- ProxySQL admin (默认 6032 端口)
INSERT INTO mysql_servers(hostgroup_id,hostname,port) VALUES (0,'127.0.0.1',3306);
LOAD MYSQL SERVERS TO RUNTIME; SAVE MYSQL SERVERS TO DISK;

-- JuiceFS 使用的 DB 账号（让 ProxySQL 用同一账号转发）
INSERT INTO mysql_users(username,password,default_hostgroup) VALUES ('<jfs_user>','<jfs_pass>',0);
LOAD MYSQL USERS TO RUNTIME; SAVE MYSQL USERS TO DISK;
```

### 2. 找到 statfs 查询的 digest

让流量先经过 ProxySQL 几秒，然后：

```sql
SELECT digest, digest_text, count_star
FROM stats_mysql_query_digest
WHERE digest_text LIKE '%counter%'
ORDER BY count_star DESC;
```

你会看到一条 `count_star` 极高的：`SELECT ?,? FROM jfs_counter WHERE ?=? LIMIT ?`（usedSpace 与 totalInodes 因字面量被规约成 `?`，**共用同一个 digest**）。记下它的 `digest`。

### 3. 给该 digest 加 TTL 缓存规则

```sql
-- 用上一步拿到的精确 digest（推荐，最稳）
INSERT INTO mysql_query_rules(rule_id,active,digest,cache_ttl,apply)
VALUES (100, 1, '<上一步的 digest>', 2000, 1);   -- 缓存 2000ms

-- 或者用正则兜底（覆盖自定义前缀）
-- INSERT INTO mysql_query_rules(rule_id,active,match_digest,cache_ttl,apply)
-- VALUES (100, 1, '^SELECT .* FROM jfs(_[a-z0-9]+)?_?counter WHERE .* LIMIT', 2000, 1);

LOAD MYSQL QUERY RULES TO RUNTIME; SAVE MYSQL QUERY RULES TO DISK;
```

效果：~25 万 QPS 被压成 ~1~2 QPS 打到 MySQL；TTL 内的陈旧度（≤2s）远小于客户端自身 12s 的刷新粒度，statfs/df 完全可接受。写操作、`nextInode` 分配（走 `FOR UPDATE`，是另一个 digest）等全部原样穿透，不受影响。

### 4. 切流量与验证

- 将挂载点用的 MySQL 域名/VIP 指向 ProxySQL（DNS 或 VIP 漂移，不动挂载配置）。
- 验证缓存命中：

```sql
SELECT count_star, sum_time FROM stats_mysql_query_digest WHERE digest='<digest>';
-- 观察后端 MySQL 上该 digest 的增速骤降；ProxySQL 侧 stats_mysql_global 里 Query_Cache_* 计数上涨。
```

- 观察 MySQL CPU 与正常元数据请求延迟回落。

> 进阶：若有只读副本，可在同一条规则里把该 digest 的 `destination_hostgroup` 指到副本读组，进一步把 statfs 读流量从主库剥离（statfs 是只读，容忍副本延迟）。

---

## 过渡期止血（ProxySQL 还没就位时，纯 mysqld）

纯 mysqld 无法合并这些查询，只能**防止彻底熔断**，代价是会一并限制正常请求，仅作短时过渡：

- 部署/启用线程池（Percona/MySQL Enterprise thread pool），用 `thread_pool_size` 限制并发执行线程，避免 25 万并发把 CPU 拖入上下文切换风暴。
- 适当设置 `innodb_thread_concurrency` 限制并发进入 InnoDB 的线程数，给真实事务留出执行窗口。
- 确认 `innodb_purge_threads` 足够、purge 没有滞后（热行版本链长会放大每次读的成本）。

> **不要**把 `jfs_counter` 改成 `ENGINE=MEMORY`：MEMORY 表在 MySQL 重启后数据清零，会丢掉 `nextInode`/`nextChunk` 等关键计数器，导致 ID 复用与元数据损坏。

---

## 事后根因修复（恢复后再做，需滚动重挂）

1. **滚动重挂带 `--fast-statfs`**：开启后 statfs 走进程内、由心跳（默认 12s）刷新的内存用量，热路径**完全不打 DB**（`base.go:1162`）。精度下降到秒级/十几秒级，对 df 可接受。这是最干净的根因修复，但需要重挂（受当前"不能改挂载参数"约束，故放在事后）。
2. 若希望进一步降低心跳期间的偏差，可结合调度让大规模挂载点的 statfs/心跳错峰。
3. 长期可在客户端侧对 statfs 增加合并/缓存（社区改进方向），从源头杜绝 N 个挂载点对同一计数器的重复读。

---

## 回滚

- 删除 ProxySQL 缓存规则：`DELETE FROM mysql_query_rules WHERE rule_id=100; LOAD MYSQL QUERY RULES TO RUNTIME;`
- 将 VIP/DNS 切回直连 MySQL。
- 过渡期调整的 `innodb_thread_concurrency` 等改回原值。
