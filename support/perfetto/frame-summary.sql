CREATE PERFETTO TABLE vtb_frame_events AS
SELECT
  s.id,
  s.parent_id,
  coalesce(pt.upid, t.upid) AS upid,
  CASE s.name WHEN 'render.nv12_ready' THEN 'render.output_ready' ELSE s.name END AS name,
  s.ts,
  s.dur,
  extract_arg(s.arg_set_id, 'debug.frame_id') AS frame_id,
  extract_arg(s.arg_set_id, 'debug.snapshot_generation') AS snapshot_generation,
  extract_arg(s.arg_set_id, 'debug.sequence') AS sequence,
  extract_arg(s.arg_set_id, 'debug.new_snapshot') AS new_snapshot,
  extract_arg(s.arg_set_id, 'debug.tracking_valid') AS tracking_valid
FROM slice s
LEFT JOIN process_track pt ON pt.id = s.track_id
LEFT JOIN thread_track tt ON tt.id = s.track_id
LEFT JOIN thread t ON t.utid = tt.utid
WHERE s.category = 'vtb.thread_frame_trace_flow';

-- Every set of rates belongs to one Live2D process, even if it restarted.
SELECT
  p.pid,
  e.name AS stage,
  count(*) AS events,
  round((count(*) - 1) * 1e9 / nullif(max(e.ts) - min(e.ts), 0), 2) AS fps
FROM vtb_frame_events e
JOIN process p USING (upid)
WHERE e.name IN ('v4l2.dequeue', 'tracking.frame', 'render.output_ready')
GROUP BY e.upid, e.name
ORDER BY p.pid, min(e.ts);

SELECT p.pid, e.name AS event, count(*) AS events
FROM vtb_frame_events e
JOIN process p USING (upid)
WHERE e.name IN ('capture.drop_oldest', 'tracking.drop_oldest', 'render.rgba_pool_full')
GROUP BY e.upid, e.name
ORDER BY p.pid, e.name;

-- Select the first render output attributed to each camera frame. Generation
-- zero is an initial render without a tracking snapshot.
CREATE PERFETTO TABLE vtb_frame_links AS
WITH candidates AS (
  SELECT
    input.upid,
    input.frame_id,
    rendered.sequence,
    queued.ts - input.ts AS capture_dispatch_ns,
    tracking.ts - queued.ts AS capture_queue_ns,
    consumed.ts - snapshot.ts AS render_queue_ns,
    rendered.ts - input.ts AS latency_ns,
    rendered.tracking_valid,
    row_number() OVER (PARTITION BY input.upid, input.frame_id ORDER BY rendered.ts) AS output_rank
  FROM vtb_frame_events input
  JOIN vtb_frame_events queued USING (upid, frame_id)
  JOIN vtb_frame_events tracking USING (upid, frame_id)
  JOIN vtb_frame_events snapshot USING (upid, frame_id)
  JOIN vtb_frame_events consumed
    ON consumed.upid = snapshot.upid AND consumed.snapshot_generation = snapshot.snapshot_generation
  JOIN vtb_frame_events rendered
    ON rendered.upid = consumed.upid AND rendered.snapshot_generation = consumed.snapshot_generation
  WHERE input.name = 'v4l2.dequeue'
    AND queued.name = 'capture.enqueue'
    AND tracking.name = 'tracking.frame'
    AND snapshot.name = 'tracking.snapshot_enqueue'
    AND consumed.name = 'render.snapshot_dequeue'
    AND rendered.name = 'render.output_ready'
    AND rendered.new_snapshot = 1
    AND input.ts <= queued.ts AND queued.ts <= tracking.ts
    AND tracking.ts <= snapshot.ts AND snapshot.ts <= consumed.ts AND consumed.ts <= rendered.ts
)
SELECT * FROM candidates WHERE output_rank = 1;

WITH durations AS (
  SELECT upid, name AS stage, dur AS ns
  FROM vtb_frame_events
  WHERE dur > 0 AND name != 'rga.acquire_destination'
  UNION ALL
  SELECT e.upid, 'rga.nv12_acquire_destination', e.dur
  FROM vtb_frame_events e
  JOIN vtb_frame_events parent ON parent.id = e.parent_id
  WHERE e.name = 'rga.acquire_destination' AND parent.name = 'rga.nv12_convert'
  UNION ALL
  SELECT upid, 'capture.dispatch', capture_dispatch_ns FROM vtb_frame_links
  UNION ALL
  SELECT upid, 'capture.queue_wait', capture_queue_ns FROM vtb_frame_links
  UNION ALL
  SELECT upid, 'render.queue_wait', render_queue_ns FROM vtb_frame_links
  UNION ALL
  SELECT upid, 'v4l2.dequeue_to_first_linked_render', latency_ns FROM vtb_frame_links
)
SELECT
  p.pid,
  d.stage,
  count(*) AS samples,
  round(percentile(d.ns, 50) / 1e6, 3) AS p50_ms,
  round(percentile(d.ns, 95) / 1e6, 3) AS p95_ms
FROM durations d
JOIN process p USING (upid)
GROUP BY d.upid, d.stage
ORDER BY p.pid, d.stage;

SELECT
  p.pid,
  count(*) AS linked_camera_frames,
  sum(tracking_valid) AS with_valid_tracking,
  min(frame_id) AS first_linked_camera_frame,
  min(sequence) AS first_linked_render_sequence
FROM vtb_frame_links
JOIN process p USING (upid)
GROUP BY upid;

SELECT name, idx AS buffer, value
FROM stats
WHERE value > 0
  AND (severity = 'data_loss' OR name = 'traced_buf_trace_writer_packet_loss')
ORDER BY name, idx;
