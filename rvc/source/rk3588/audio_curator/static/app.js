const PAGE_SIZE = 100;

const state = {
  items: [],
  total: 0,
  offset: 0,
  currentIndex: 0,
  loading: false,
  noteTimer: null,
};

const elements = {
  audio: document.querySelector("#audioPlayer"),
  autoPlay: document.querySelector("#autoPlay"),
  playbackRate: document.querySelector("#playbackRate"),
  statusFilter: document.querySelector("#statusFilter"),
  splitFilter: document.querySelector("#splitFilter"),
  formatFilter: document.querySelector("#formatFilter"),
  searchInput: document.querySelector("#searchInput"),
  refreshButton: document.querySelector("#refreshButton"),
  reviewContent: document.querySelector("#reviewContent"),
  emptyState: document.querySelector("#emptyState"),
  queueList: document.querySelector("#queueList"),
  queueCount: document.querySelector("#queueCount"),
  pageLabel: document.querySelector("#pageLabel"),
  previousPage: document.querySelector("#previousPage"),
  nextPage: document.querySelector("#nextPage"),
  previousButton: document.querySelector("#previousButton"),
  nextButton: document.querySelector("#nextButton"),
  resetButton: document.querySelector("#resetButton"),
  noteInput: document.querySelector("#noteInput"),
  saveState: document.querySelector("#saveState"),
  toast: document.querySelector("#toast"),
};

const statusLabels = {
  unreviewed: "待筛选",
  keep: "保留",
  reject: "剔除",
  unsure: "待定",
};

function formatDuration(seconds) {
  const safeSeconds = Number(seconds || 0);
  if (safeSeconds < 60) return `${safeSeconds.toFixed(2)} 秒`;
  const minutes = Math.floor(safeSeconds / 60);
  const remainder = Math.round(safeSeconds % 60);
  return `${minutes}分${remainder}秒`;
}

function formatBytes(bytes) {
  const value = Number(bytes || 0);
  if (value < 1024 * 1024) return `${(value / 1024).toFixed(1)} KiB`;
  return `${(value / 1024 / 1024).toFixed(1)} MiB`;
}

function showToast(message, isError = false) {
  elements.toast.textContent = message;
  elements.toast.classList.toggle("error", isError);
  elements.toast.classList.add("visible");
  window.clearTimeout(showToast.timer);
  showToast.timer = window.setTimeout(() => elements.toast.classList.remove("visible"), 2200);
}

async function fetchJson(url, options = {}) {
  const response = await fetch(url, options);
  if (!response.ok) {
    let message = `${response.status} ${response.statusText}`;
    try {
      const payload = await response.json();
      if (payload.error) message = payload.error;
    } catch (_) {
      // Keep the HTTP status if the response is not JSON.
    }
    throw new Error(message);
  }
  return response.json();
}

async function loadSummary() {
  const summary = await fetchJson("/api/summary");
  const reviewed = summary.total - summary.unreviewed;
  const progress = summary.total ? (reviewed / summary.total) * 100 : 0;
  document.querySelector("#progressValue").textContent = `${progress.toFixed(1)}%`;
  document.querySelector("#progressBar").style.width = `${progress}%`;
  document.querySelector("#unreviewedCount").textContent = summary.unreviewed;
  document.querySelector("#keepCount").textContent = summary.keep_count;
  document.querySelector("#rejectCount").textContent = summary.reject_count;
  document.querySelector("#unsureCount").textContent = summary.unsure_count;
  document.querySelector("#keepDuration").textContent = formatDuration(summary.keep_duration);
}

function queryString() {
  const query = new URLSearchParams({
    status: elements.statusFilter.value,
    split: elements.splitFilter.value,
    format: elements.formatFilter.value,
    search: elements.searchInput.value.trim(),
    offset: String(state.offset),
    limit: String(PAGE_SIZE),
  });
  return query.toString();
}

async function loadItems({ preserveSelection = false } = {}) {
  if (state.loading) return;
  state.loading = true;
  const selectedItem = currentItem();
  let previousId = null;
  if (preserveSelection && selectedItem) previousId = selectedItem.id;
  try {
    const payload = await fetchJson(`/api/items?${queryString()}`);
    state.items = payload.items;
    state.total = payload.total;
    let nextIndex = 0;
    if (previousId) {
      const found = state.items.findIndex(item => item.id === previousId);
      if (found >= 0) nextIndex = found;
    }
    state.currentIndex = Math.min(nextIndex, Math.max(0, state.items.length - 1));
    renderAll();
  } catch (error) {
    showToast(`加载失败：${error.message}`, true);
  } finally {
    state.loading = false;
  }
}

function currentItem() {
  return state.items[state.currentIndex] || null;
}

function renderAll() {
  const isEmpty = state.items.length === 0;
  elements.emptyState.hidden = !isEmpty;
  elements.reviewContent.hidden = isEmpty;
  renderQueue();
  renderCurrent();
  renderPagination();
}

function renderCurrent() {
  const item = currentItem();
  if (!item) return;
  document.querySelector("#clipPosition").textContent = `当前页第 ${state.currentIndex + 1} 条 · 全部第 ${state.offset + state.currentIndex + 1} 条`;
  document.querySelector("#clipName").textContent = item.file_name;
  document.querySelector("#clipPath").textContent = item.relative_path;
  document.querySelector("#formatLabel").textContent = item.format_label;
  document.querySelector("#durationLabel").textContent = formatDuration(item.duration);
  document.querySelector("#sizeLabel").textContent = formatBytes(item.size_bytes);
  document.querySelector("#relatedLabel").textContent = `同编号版本 ${item.related_count} 个`;
  elements.noteInput.value = item.note || "";

  const badge = document.querySelector("#statusBadge");
  badge.textContent = statusLabels[item.status];
  badge.className = `status-badge ${item.status}`;
  document.querySelectorAll(".decision").forEach(button => {
    button.classList.toggle("active", button.dataset.status === item.status);
  });

  const previousSource = elements.audio.dataset.itemId;
  elements.audio.playbackRate = Number(elements.playbackRate.value);
  if (previousSource !== item.id) {
    elements.audio.src = item.audio_url;
    elements.audio.dataset.itemId = item.id;
    elements.audio.load();
    if (elements.autoPlay.checked) {
      elements.audio.play().catch(() => {});
    }
  }
  elements.previousButton.disabled = state.offset === 0 && state.currentIndex === 0;
  elements.nextButton.disabled = state.offset + state.currentIndex >= state.total - 1;
}

function renderQueue() {
  elements.queueList.replaceChildren();
  state.items.forEach((item, index) => {
    const listItem = document.createElement("li");
    const button = document.createElement("button");
    button.type = "button";
    button.className = "queue-item";
    if (index === state.currentIndex) button.classList.add("active");
    button.addEventListener("click", () => selectIndex(index));

    const dot = document.createElement("span");
    dot.className = `queue-dot ${item.status}`;
    const name = document.createElement("span");
    name.className = "queue-name";
    name.textContent = item.file_name;
    const duration = document.createElement("span");
    duration.className = "queue-duration";
    duration.textContent = `${Number(item.duration).toFixed(1)}s`;
    button.append(dot, name, duration);
    listItem.append(button);
    elements.queueList.append(listItem);
  });
  elements.queueCount.textContent = `${state.total} 条`;
}

function renderPagination() {
  const page = Math.floor(state.offset / PAGE_SIZE) + 1;
  const pageCount = Math.max(1, Math.ceil(state.total / PAGE_SIZE));
  elements.pageLabel.textContent = `${page} / ${pageCount} 页`;
  elements.previousPage.disabled = state.offset === 0;
  elements.nextPage.disabled = state.offset + PAGE_SIZE >= state.total;
}

function selectIndex(index) {
  if (index < 0 || index >= state.items.length) return;
  flushNote();
  state.currentIndex = index;
  renderCurrent();
  renderQueue();
}

async function updateReview(status, note = elements.noteInput.value, moveAfter = true) {
  const item = currentItem();
  if (!item) return;
  elements.saveState.textContent = "保存中…";
  elements.saveState.className = "save-state saving";
  try {
    const result = await fetchJson(`/api/items/${item.id}/review`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ status, note }),
    });
    item.status = result.status;
    item.note = result.note;
    elements.saveState.textContent = "已自动保存";
    elements.saveState.className = "save-state";
    await loadSummary();

    if (moveAfter) {
      if (elements.statusFilter.value === "unreviewed") {
        await loadItems();
      } else {
        renderCurrent();
        renderQueue();
        await moveNext();
      }
    } else {
      renderCurrent();
      renderQueue();
    }
  } catch (error) {
    elements.saveState.textContent = "保存失败";
    elements.saveState.className = "save-state error";
    showToast(`保存失败：${error.message}`, true);
  }
}

function scheduleNoteSave() {
  window.clearTimeout(state.noteTimer);
  state.noteTimer = window.setTimeout(() => {
    const item = currentItem();
    if (item && elements.noteInput.value !== item.note) {
      updateReview(item.status, elements.noteInput.value, false);
    }
  }, 700);
}

function flushNote() {
  window.clearTimeout(state.noteTimer);
  const item = currentItem();
  if (item && elements.noteInput.value !== item.note) {
    updateReview(item.status, elements.noteInput.value, false);
  }
}

async function moveNext() {
  if (state.currentIndex + 1 < state.items.length) {
    selectIndex(state.currentIndex + 1);
    return;
  }
  if (state.offset + PAGE_SIZE < state.total) {
    state.offset += PAGE_SIZE;
    await loadItems();
  }
}

async function movePrevious() {
  if (state.currentIndex > 0) {
    selectIndex(state.currentIndex - 1);
    return;
  }
  if (state.offset > 0) {
    state.offset = Math.max(0, state.offset - PAGE_SIZE);
    await loadItems();
    selectIndex(state.items.length - 1);
  }
}

function resetFiltersAndLoad() {
  state.offset = 0;
  loadItems();
}

document.querySelectorAll(".decision").forEach(button => {
  button.addEventListener("click", () => updateReview(button.dataset.status));
});
elements.resetButton.addEventListener("click", () => updateReview("unreviewed", elements.noteInput.value, false));
elements.nextButton.addEventListener("click", moveNext);
elements.previousButton.addEventListener("click", movePrevious);
elements.previousPage.addEventListener("click", () => {
  state.offset = Math.max(0, state.offset - PAGE_SIZE);
  loadItems();
});
elements.nextPage.addEventListener("click", () => {
  state.offset += PAGE_SIZE;
  loadItems();
});
elements.refreshButton.addEventListener("click", () => loadItems({ preserveSelection: true }));
elements.statusFilter.addEventListener("change", resetFiltersAndLoad);
elements.splitFilter.addEventListener("change", resetFiltersAndLoad);
elements.formatFilter.addEventListener("change", resetFiltersAndLoad);
elements.noteInput.addEventListener("input", scheduleNoteSave);
elements.searchInput.addEventListener("input", () => {
  window.clearTimeout(elements.searchInput.timer);
  elements.searchInput.timer = window.setTimeout(resetFiltersAndLoad, 350);
});
elements.playbackRate.addEventListener("change", () => {
  elements.audio.playbackRate = Number(elements.playbackRate.value);
});

document.addEventListener("keydown", event => {
  const target = event.target;
  const isTyping = target instanceof HTMLInputElement || target instanceof HTMLTextAreaElement || target instanceof HTMLSelectElement;
  if (isTyping) return;
  if (event.code === "Space") {
    event.preventDefault();
    if (elements.audio.paused) elements.audio.play().catch(() => {});
    else elements.audio.pause();
  }
  if (event.key === "1") updateReview("keep");
  if (event.key === "2") updateReview("reject");
  if (event.key === "3") updateReview("unsure");
  if (event.key.toLowerCase() === "r") {
    elements.audio.currentTime = 0;
    elements.audio.play().catch(() => {});
  }
  if (event.key === "ArrowRight") moveNext();
  if (event.key === "ArrowLeft") movePrevious();
});

window.addEventListener("beforeunload", flushNote);

Promise.all([loadSummary(), loadItems()]).catch(error => showToast(`初始化失败：${error.message}`, true));
