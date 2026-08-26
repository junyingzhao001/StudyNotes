-- Android Performance Analyzer / PerfettoSQL 启动分析查询示例
-- 说明：
-- 1. 时间戳 ts、时长 dur 的单位通常为纳秒，除以 1e6 转换为毫秒。
-- 2. 表结构会随 Perfetto 版本演进；如果某条查询报字段不存在，
--    可先执行：SELECT * FROM <table_name> LIMIT 1;
-- 3. 示例包名、进程名均已泛化，请替换为实际测试应用。

-- ============================================================
-- 1. 查看 Trace 覆盖的总时间
-- ============================================================
SELECT
  start_ts / 1e6 AS start_ms,
  end_ts / 1e6 AS end_ms,
  (end_ts - start_ts) / 1e6 AS trace_duration_ms
FROM trace_bounds;

-- ============================================================
-- 2. 找到目标进程
-- ============================================================
SELECT
  upid,
  pid,
  name
FROM process
WHERE name LIKE '%example%'
ORDER BY pid;

-- ============================================================
-- 3. 找到目标进程中的主线程
-- ============================================================
SELECT
  p.name AS process_name,
  p.pid,
  t.utid,
  t.tid,
  t.name AS thread_name
FROM thread t
JOIN process p USING (upid)
WHERE p.name LIKE '%example%'
  AND (t.name = 'main' OR t.tid = p.pid);

-- ============================================================
-- 4. 查询主线程耗时最长的 Slice
-- ============================================================
SELECT
  s.name,
  s.ts / 1e6 AS start_ms,
  s.dur / 1e6 AS duration_ms
FROM slice s
JOIN thread_track tt ON s.track_id = tt.id
JOIN thread t USING (utid)
JOIN process p USING (upid)
WHERE p.name LIKE '%example%'
  AND (t.name = 'main' OR t.tid = p.pid)
  AND s.dur > 0
ORDER BY s.dur DESC
LIMIT 100;

-- ============================================================
-- 5. 查询常见启动阶段 Slice
-- 注意：Slice 名称取决于系统版本、应用埋点和采集配置。
-- ============================================================
SELECT
  s.name,
  s.ts / 1e6 AS start_ms,
  s.dur / 1e6 AS duration_ms
FROM slice s
WHERE s.name GLOB '*bindApplication*'
   OR s.name GLOB '*Activity*onCreate*'
   OR s.name GLOB '*Choreographer#doFrame*'
   OR s.name GLOB '*inflate*'
   OR s.name GLOB '*ContentProvider*'
ORDER BY s.ts;

-- ============================================================
-- 6. 主线程各调度状态耗时汇总
-- 常见 state：
--   Running：正在 CPU 上运行
--   Runnable：可运行，但正在等待 CPU
--   S / Sleeping：休眠或等待
--   D：不可中断睡眠，常与 I/O 等待有关
-- 具体显示值以当前 Trace 为准。
-- ============================================================
SELECT
  ts.state,
  COUNT(*) AS sample_count,
  SUM(ts.dur) / 1e6 AS total_duration_ms,
  MAX(ts.dur) / 1e6 AS max_duration_ms
FROM thread_state ts
JOIN thread t USING (utid)
JOIN process p USING (upid)
WHERE p.name LIKE '%example%'
  AND (t.name = 'main' OR t.tid = p.pid)
  AND ts.dur > 0
GROUP BY ts.state
ORDER BY SUM(ts.dur) DESC;

-- ============================================================
-- 7. 主线程最长的非 Running 状态
-- 用于寻找长时间锁等待、I/O 等待、调度等待。
-- ============================================================
SELECT
  ts.ts / 1e6 AS start_ms,
  ts.dur / 1e6 AS duration_ms,
  ts.state,
  ts.io_wait
FROM thread_state ts
JOIN thread t USING (utid)
JOIN process p USING (upid)
WHERE p.name LIKE '%example%'
  AND (t.name = 'main' OR t.tid = p.pid)
  AND ts.state != 'Running'
  AND ts.dur > 0
ORDER BY ts.dur DESC
LIMIT 100;

-- ============================================================
-- 8. 启动窗口内的 GC Slice
-- 先将 start_ns / end_ns 替换成目标窗口的纳秒时间戳。
-- ============================================================
WITH startup_window AS (
  SELECT
    0 AS start_ns,
    9223372036854775807 AS end_ns
)
SELECT
  s.name,
  s.ts / 1e6 AS start_ms,
  s.dur / 1e6 AS duration_ms
FROM slice s, startup_window w
WHERE s.ts BETWEEN w.start_ns AND w.end_ns
  AND (s.name GLOB '*GC*' OR s.name GLOB '*gc*')
ORDER BY s.dur DESC;

-- ============================================================
-- 9. 查找自定义 Trace 埋点
-- 对应代码中的 Trace.beginSection("AppInit/xxx")。
-- ============================================================
SELECT
  s.name,
  s.ts / 1e6 AS start_ms,
  s.dur / 1e6 AS duration_ms
FROM slice s
WHERE s.name GLOB 'AppInit/*'
ORDER BY s.ts;

-- ============================================================
-- 10. 每 16.67 ms 窗口内主线程 Slice 总耗时（辅助分析 60 Hz 卡顿）
-- 这不是最终的帧指标，只用于快速发现工作密集区。
-- ============================================================
SELECT
  CAST((s.ts / 1000000.0) / 16.67 AS INT) AS window_id,
  MIN(s.ts) / 1e6 AS window_start_ms,
  SUM(s.dur) / 1e6 AS summed_slice_duration_ms,
  COUNT(*) AS slice_count
FROM slice s
JOIN thread_track tt ON s.track_id = tt.id
JOIN thread t USING (utid)
JOIN process p USING (upid)
WHERE p.name LIKE '%example%'
  AND (t.name = 'main' OR t.tid = p.pid)
  AND s.dur > 0
GROUP BY window_id
ORDER BY summed_slice_duration_ms DESC
LIMIT 100;

