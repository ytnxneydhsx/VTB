"use strict";

const perfettoOrigin = "https://ui.perfetto.dev";
const timeline = document.getElementById("timeline");
const refreshButton = document.getElementById("refresh");
const statusLabel = document.getElementById("status");
const timelineStatus = document.getElementById("timeline-status");
const framePath = document.getElementById("frame-path");

const number = new Intl.NumberFormat("zh-CN");
const fps = new Intl.NumberFormat("zh-CN", {minimumFractionDigits: 2, maximumFractionDigits: 2});
const millis = new Intl.NumberFormat("zh-CN", {minimumFractionDigits: 3, maximumFractionDigits: 3});

let perfettoReady = false;
let request;
let latestTrace;

function showTrace() {
  if (!perfettoReady || !latestTrace) return;

  const {buffer, time} = latestTrace;
  timeline.contentWindow.postMessage({
    perfetto: {
      buffer,
      title: `Live2D RK3588 · ${time}`,
      keepApiOpen: true,
    },
  }, perfettoOrigin, [buffer]);
  latestTrace = undefined;
  timelineStatus.textContent = `时间线 ${time}`;
}

function showSummary(summary) {
  for (const card of document.querySelectorAll("[data-rate]")) {
    const rate = summary.rates[card.dataset.rate];
    card.querySelector(".value").textContent = rate?.fps == null ? "—" : fps.format(rate.fps);
    card.querySelector(".count").textContent = `${number.format(rate?.frames ?? 0)} 帧`;
  }
  for (const row of document.querySelectorAll("[data-stage]")) {
    const stage = summary.latency[row.dataset.stage];
    row.querySelector(".p50").textContent = stage ? millis.format(stage.p50) : "—";
    row.querySelector(".p95").textContent = stage ? millis.format(stage.p95) : "—";
    row.querySelector(".samples").textContent = number.format(stage?.samples ?? 0);
  }
  const latency = summary.latency.capture_to_nv12;
  document.getElementById("latency").textContent = latency ? millis.format(latency.p95) : "—";
  document.getElementById("linked").textContent =
    `${number.format(summary.linked)} 帧匹配 · ${number.format(summary.valid)} 帧跟踪有效`;
  document.getElementById("drops").textContent =
    `队列淘汰 · 采集 ${number.format(summary.capture_drops)} · 快照 ${number.format(summary.snapshot_drops)}`;
  const example = summary.example;
  framePath.textContent = example
    ? `摄像头 #${example.frame} → 跟踪快照 #${example.snapshot} → 首张 NV12 #${example.output} · ${millis.format(example.ms)} ms`
    : "本片段没有完整匹配的摄像头帧。";
}

async function refresh() {
  if (request) return;

  const controller = new AbortController();
  request = controller;
  refreshButton.disabled = true;
  refreshButton.textContent = "采集中…";
  statusLabel.textContent = "正在采集 RK3588 的 9 秒片段…";
  try {
    const reply = await fetch("/api/window", {
      method: "POST",
      cache: "no-store",
      signal: controller.signal,
    });
    if (reply.status === 204) {
      statusLabel.textContent = "本片段没有 Live2D 帧。";
      return;
    }
    if (!reply.ok) throw new Error(await reply.text());

    const summary = JSON.parse(atob(reply.headers.get("X-VTB-Frame-Summary")));
    const buffer = await reply.arrayBuffer();
    if (controller.signal.aborted) return;

    const time = new Date().toLocaleTimeString("zh-CN", {hour12: false});
    showSummary(summary);
    latestTrace = {buffer, time};
    showTrace();
    statusLabel.textContent = `${time} 已更新 · 9 秒采样 · 手动刷新`;
  } catch (error) {
    if (!controller.signal.aborted) {
      statusLabel.textContent = error instanceof TypeError ? "无法连接 Mac 本地入口。" : error.message;
    }
  } finally {
    request = undefined;
    refreshButton.disabled = false;
    refreshButton.textContent = "刷新片段";
  }
}

function pingPerfetto() {
  timeline.contentWindow.postMessage("PING", perfettoOrigin);
}

const handshake = setInterval(pingPerfetto, 250);
window.addEventListener("message", (event) => {
  if (event.origin !== perfettoOrigin || event.source !== timeline.contentWindow ||
      event.data !== "PONG" || perfettoReady) return;

  perfettoReady = true;
  clearInterval(handshake);
  showTrace();
});

window.addEventListener("pagehide", () => request?.abort());
refreshButton.addEventListener("click", refresh);
pingPerfetto();
refresh();
