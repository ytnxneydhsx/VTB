-- One JSON result for the Mac viewer. Restrict the window to one Live2D process.
CREATE PERFETTO TABLE vtb_viewer_events AS
WITH events AS (
  SELECT s.id, coalesce(pt.upid, t.upid) AS upid, s.name, s.ts, s.dur, s.arg_set_id
  FROM slice s
  LEFT JOIN process_track pt ON pt.id = s.track_id
  LEFT JOIN thread_track tt ON tt.id = s.track_id
  LEFT JOIN thread t ON t.utid = tt.utid
  WHERE s.category = 'vtb.thread_frame_trace_flow'
)
SELECT
  name, ts, dur,
  extract_arg(arg_set_id, 'debug.frame_id') AS frame_id,
  extract_arg(arg_set_id, 'debug.snapshot_generation') AS snapshot_generation,
  extract_arg(arg_set_id, 'debug.sequence') AS sequence,
  extract_arg(arg_set_id, 'debug.new_snapshot') AS new_snapshot,
  extract_arg(arg_set_id, 'debug.tracking_valid') AS tracking_valid
FROM events
WHERE upid = (
  SELECT upid FROM events WHERE name IN ('v4l2.dequeue', 'render.nv12_ready')
  ORDER BY ts DESC, id DESC LIMIT 1
);

WITH
  rates AS (
    SELECT name, count(*) AS frames,
           round((count(*) - 1) * 1e9 / nullif(max(ts) - min(ts), 0), 2) AS fps
    FROM vtb_viewer_events
    WHERE name IN ('v4l2.dequeue', 'tracking.frame', 'render.nv12_ready')
    GROUP BY name
  ),
  candidates AS (
    SELECT camera.frame_id, output.snapshot_generation, output.sequence, output.tracking_valid,
           tracking.ts - queued.ts AS capture_queue_ns,
           consumed.ts - snapshot.ts AS render_queue_ns,
           output.ts - camera.ts AS total_ns,
           output.ts AS output_ts,
           row_number() OVER (
             PARTITION BY camera.frame_id ORDER BY output.ts, output.sequence
           ) AS output_rank
    FROM vtb_viewer_events camera
    JOIN vtb_viewer_events queued USING (frame_id)
    JOIN vtb_viewer_events tracking USING (frame_id)
    JOIN vtb_viewer_events snapshot USING (frame_id)
    JOIN vtb_viewer_events consumed ON consumed.snapshot_generation = snapshot.snapshot_generation
    JOIN vtb_viewer_events output ON output.snapshot_generation = snapshot.snapshot_generation
    WHERE camera.name = 'v4l2.dequeue' AND queued.name = 'capture.enqueue'
      AND tracking.name = 'tracking.frame' AND snapshot.name = 'tracking.snapshot_enqueue'
      AND consumed.name = 'render.snapshot_dequeue' AND output.name = 'render.nv12_ready'
      AND output.new_snapshot = 1 AND output.snapshot_generation > 0
      AND camera.ts <= queued.ts AND queued.ts <= tracking.ts AND tracking.ts <= snapshot.ts
      AND snapshot.ts <= consumed.ts AND consumed.ts <= output.ts
  ),
  linked AS (
    SELECT * FROM candidates WHERE output_rank = 1
  ),
  durations AS (
    SELECT name, dur AS ns
    FROM vtb_viewer_events
    WHERE dur > 0 AND name IN (
      'tracking.frame', 'tracking.rknn_detector', 'tracking.rknn_landmarks',
      'render.wait_gpu', 'rga.nv12_sync'
    )
    UNION ALL SELECT 'capture.queue_wait', capture_queue_ns FROM linked
    UNION ALL SELECT 'render.queue_wait', render_queue_ns FROM linked
    UNION ALL SELECT 'capture_to_nv12', total_ns FROM linked
  ),
  latency AS (
    SELECT name, count(*) AS samples, round(percentile(ns, 50) / 1e6, 3) AS p50,
           round(percentile(ns, 95) / 1e6, 3) AS p95
    FROM durations GROUP BY name
  )
SELECT hex(json_object(
  'rates', (SELECT json_group_object(name, json_object('frames', frames, 'fps', fps)) FROM rates),
  'latency', (
    SELECT json_group_object(name, json_object('samples', samples, 'p50', p50, 'p95', p95))
    FROM latency
  ),
  'linked', (SELECT count(*) FROM linked),
  'valid', (SELECT count(*) FROM linked WHERE tracking_valid = 1),
  'example', json((
    SELECT json_object(
      'frame', frame_id, 'snapshot', snapshot_generation, 'output', sequence,
      'ms', round(total_ns / 1e6, 3)
    )
    FROM linked ORDER BY output_ts DESC, sequence DESC LIMIT 1
  )),
  'capture_drops', (SELECT count(*) FROM vtb_viewer_events WHERE name = 'capture.drop_oldest'),
  'snapshot_drops', (SELECT count(*) FROM vtb_viewer_events WHERE name = 'tracking.drop_oldest')
)) AS summary_hex;
