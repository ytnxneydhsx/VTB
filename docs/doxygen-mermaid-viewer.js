(function () {
    "use strict";

    const style = document.createElement("style");
    style.textContent = `
        .mermaidgraph.vtb-zoomable-diagram {
            position: relative;
            cursor: zoom-in;
            border-radius: 8px;
        }
        .mermaidgraph.vtb-zoomable-diagram:focus-visible {
            outline: 2px solid #58a6ff;
            outline-offset: 4px;
        }
        .vtb-diagram-open {
            position: absolute;
            top: 10px;
            right: 10px;
            z-index: 2;
            padding: 7px 11px;
            border: 1px solid #8b949e;
            border-radius: 6px;
            color: #f0f6fc;
            background: rgba(22, 27, 34, 0.88);
            font: 13px/1.2 sans-serif;
            cursor: pointer;
        }
        .vtb-diagram-open:hover {
            background: #30363d;
        }
        .vtb-diagram-modal {
            position: fixed;
            inset: 0;
            z-index: 10000;
            display: grid;
            grid-template-rows: auto 1fr;
            background: rgba(1, 4, 9, 0.96);
        }
        .vtb-diagram-toolbar {
            display: flex;
            align-items: center;
            justify-content: space-between;
            gap: 16px;
            padding: 12px 18px;
            color: #f0f6fc;
            background: #161b22;
            border-bottom: 1px solid #30363d;
            font: 14px/1.4 sans-serif;
        }
        .vtb-diagram-close {
            padding: 7px 12px;
            border: 1px solid #8b949e;
            border-radius: 6px;
            color: #f0f6fc;
            background: #21262d;
            cursor: pointer;
        }
        .vtb-diagram-stage {
            position: relative;
            overflow: hidden;
            touch-action: none;
            cursor: grab;
        }
        .vtb-diagram-stage.vtb-dragging {
            cursor: grabbing;
        }
        .vtb-diagram-surface {
            position: absolute;
            left: 0;
            top: 0;
            transform-origin: 0 0;
            will-change: transform;
        }
        .vtb-diagram-surface svg {
            display: block;
            max-width: none !important;
            max-height: none !important;
        }
    `;
    document.head.appendChild(style);

    const readinessEventLabels = new Set([
        "Buffer 可读",
        "condition_.notify_one()：通知最新帧可取",
        "not_empty_.notify_one()：通知最新帧可取",
        "condition_.notify_all()：通知等待线程完成退出"
    ]);
    const readinessEventColor = "#58a6ff";
    const controlFlowMessageLabels = new Set([
        "判断本帧路径：计算 must_detect",
        "跟踪失效：reset_tracking()",
        "检测成功：保存检测框和检测置信度",
        "关键点可信：保存本帧 updated_box 给下一帧",
        "发布映射快照：保存 latest_mapping_ 和 generation",
        "执行 Session 内的 generation 比较",
        "检查 read() 结果：发布 Lease 或结束采集",
        "检查 frame_：保存新 Lease，并记录是否替换旧 Lease",
        "检查 frame_ / finished_：等待、交付或结束"
    ]);
    const controlFlowColor = "#e3b341";
    const threadHandoffMessageLabels = new Set([
        "wait_and_take()",
        "wait_pop()：无帧且未结束时阻塞",
        "try_pop()（非阻塞）",
        "publish(move(frame))",
        "唤醒并 move(最新租约)",
        "move(最新 DecodedFrameLease)",
        "publish(Live2DTrackingSnapshot)",
        "发布最新 Live2D 状态（处理线程 → 交接类）",
        "Live2DTrackingSnapshot（generation 已变化）",
        "optional<Live2DTrackingSnapshot>"
    ]);
    const threadHandoffMessagePrefixes = [
        "publish(move(frame))",
        "wait_and_take()",
        "wait_pop()",
        "try_pop()",
        "move(最新 DecodedFrameLease)",
        "publish(Live2DTrackingSnapshot)",
        "publish_mapping(mapping)",
        "read_snapshot_if_changed()",
        "optional<Live2DTrackingSnapshot>"
    ];
    const threadHandoffActorNames = new Set([
        "Slot",
        "TrackingChannel",
        "InputSlot"
    ]);
    const threadHandoffColor = "#ff5d5d";
    const threadHandoffFill = "#7f1d1d";

    function colorMessages(
        graph,
        labelsToColor,
        color,
        markerSuffix,
        forceSolid,
        labelPrefixes = []
    ) {
        const svg = graph.querySelector("svg");
        if (!svg) {
            return;
        }

        const labels = Array.from(svg.querySelectorAll(".messageText"));
        const lines = Array.from(
            svg.querySelectorAll(".messageLine0, .messageLine1")
        );

        labels.forEach(function (label, index) {
            const labelText = label.textContent.trim();
            const prefixMatch = labelPrefixes.some(function (prefix) {
                return labelText.startsWith(prefix);
            });
            if (!labelsToColor.has(labelText) && !prefixMatch) {
                return;
            }
            const styledAttribute = `data-vtb-message-${markerSuffix}`;
            if (label.getAttribute(styledAttribute) === "true") {
                return;
            }

            const line = lines[index];
            if (!line) {
                return;
            }

            label.setAttribute(styledAttribute, "true");
            label.style.fill = color;
            label.style.fontWeight = "600";
            line.style.stroke = color;
            if (forceSolid) {
                line.style.strokeDasharray = "none";
            }

            const markerReference = line.getAttribute("marker-end");
            const markerMatch = markerReference && markerReference.match(/#([^\)]+)/);
            if (!markerMatch) {
                return;
            }

            const markerId = markerMatch[1];
            const coloredMarkerId = `${markerId}-vtb-${markerSuffix}`;
            let coloredMarker = Array.from(svg.querySelectorAll("marker")).find(
                function (marker) {
                    return marker.id === coloredMarkerId;
                }
            );
            if (!coloredMarker) {
                const marker = Array.from(svg.querySelectorAll("marker")).find(
                    function (candidate) {
                        return candidate.id === markerId;
                    }
                );
                if (!marker) {
                    return;
                }

                coloredMarker = marker.cloneNode(true);
                coloredMarker.id = coloredMarkerId;
                coloredMarker.querySelectorAll("path, polygon").forEach(
                    function (shape) {
                        shape.style.fill = color;
                        shape.style.stroke = color;
                    }
                );
                marker.parentNode.appendChild(coloredMarker);
            }
            line.setAttribute("marker-end", `url(#${coloredMarkerId})`);
        });
    }

    function colorThreadHandoffBoundary(graph) {
        const svg = graph.querySelector("svg");
        if (!svg) {
            return;
        }

        svg.querySelectorAll("rect.actor").forEach(function (box) {
            if (!threadHandoffActorNames.has(box.getAttribute("name"))) {
                return;
            }
            box.style.fill = threadHandoffFill;
            box.style.stroke = threadHandoffColor;
            box.style.strokeWidth = "2px";
            const label = box.parentElement.querySelector("text.actor-box");
            if (label) {
                label.style.fill = "#ffffff";
                label.style.fontWeight = "600";
            }
        });

        svg.querySelectorAll(".actor-line").forEach(function (line) {
            if (!threadHandoffActorNames.has(line.getAttribute("data-id"))) {
                return;
            }
            line.style.stroke = threadHandoffColor;
            line.style.strokeWidth = "2px";
        });
    }

    function findRenderedSvg(graph) {
        return graph.querySelector("pre.mermaid > svg[viewBox]");
    }

    function openViewer(graph) {
        const sourceSvg = findRenderedSvg(graph);
        if (!sourceSvg) {
            return;
        }

        const modal = document.createElement("div");
        modal.className = "vtb-diagram-modal";
        modal.setAttribute("role", "dialog");
        modal.setAttribute("aria-modal", "true");
        modal.setAttribute("aria-label", "图表放大查看");

        const toolbar = document.createElement("div");
        toolbar.className = "vtb-diagram-toolbar";
        const hint = document.createElement("span");
        hint.textContent = "滚轮缩放 · 拖拽平移 · 双击复位 · Esc 关闭";
        const close = document.createElement("button");
        close.className = "vtb-diagram-close";
        close.type = "button";
        close.textContent = "关闭";
        toolbar.append(hint, close);

        const stage = document.createElement("div");
        stage.className = "vtb-diagram-stage";
        const surface = document.createElement("div");
        surface.className = "vtb-diagram-surface";
        const svg = sourceSvg.cloneNode(true);
        surface.appendChild(svg);
        stage.appendChild(surface);
        modal.append(toolbar, stage);
        document.body.appendChild(modal);

        const originalOverflow = document.body.style.overflow;
        document.body.style.overflow = "hidden";

        const viewBox = sourceSvg.viewBox.baseVal;
        let baseWidth = viewBox.width;
        let baseHeight = viewBox.height;
        if (baseWidth <= 0 || baseHeight <= 0) {
            const bounds = sourceSvg.getBoundingClientRect();
            baseWidth = bounds.width;
            baseHeight = bounds.height;
        }
        svg.setAttribute("width", String(baseWidth));
        svg.setAttribute("height", String(baseHeight));

        let scale = 1;
        let offsetX = 0;
        let offsetY = 0;
        let dragging = false;
        let pointerX = 0;
        let pointerY = 0;

        function render() {
            surface.style.transform =
                `translate(${offsetX}px, ${offsetY}px) scale(${scale})`;
        }

        function resetView() {
            const padding = 48;
            const availableWidth = Math.max(1, stage.clientWidth - padding * 2);
            const availableHeight = Math.max(1, stage.clientHeight - padding * 2);
            scale = Math.min(
                availableWidth / baseWidth,
                availableHeight / baseHeight,
                2
            );
            offsetX = (stage.clientWidth - baseWidth * scale) / 2;
            offsetY = (stage.clientHeight - baseHeight * scale) / 2;
            render();
        }

        function closeViewer() {
            document.body.style.overflow = originalOverflow;
            modal.remove();
            document.removeEventListener("keydown", onKeyDown);
        }

        function onKeyDown(event) {
            if (event.key === "Escape") {
                closeViewer();
            }
        }

        close.addEventListener("click", closeViewer);
        modal.addEventListener("click", function (event) {
            if (event.target === modal) {
                closeViewer();
            }
        });
        document.addEventListener("keydown", onKeyDown);

        stage.addEventListener("wheel", function (event) {
            event.preventDefault();
            const rect = stage.getBoundingClientRect();
            const cursorX = event.clientX - rect.left;
            const cursorY = event.clientY - rect.top;
            const previousScale = scale;
            const factor = Math.exp(-event.deltaY * 0.0015);
            scale = Math.min(8, Math.max(0.15, scale * factor));
            offsetX = cursorX - (cursorX - offsetX) * (scale / previousScale);
            offsetY = cursorY - (cursorY - offsetY) * (scale / previousScale);
            render();
        }, { passive: false });

        stage.addEventListener("pointerdown", function (event) {
            dragging = true;
            pointerX = event.clientX;
            pointerY = event.clientY;
            stage.classList.add("vtb-dragging");
            stage.setPointerCapture(event.pointerId);
        });
        stage.addEventListener("pointermove", function (event) {
            if (!dragging) {
                return;
            }
            offsetX += event.clientX - pointerX;
            offsetY += event.clientY - pointerY;
            pointerX = event.clientX;
            pointerY = event.clientY;
            render();
        });
        stage.addEventListener("pointerup", function (event) {
            dragging = false;
            stage.classList.remove("vtb-dragging");
            stage.releasePointerCapture(event.pointerId);
        });
        stage.addEventListener("pointercancel", function () {
            dragging = false;
            stage.classList.remove("vtb-dragging");
        });
        stage.addEventListener("dblclick", resetView);
        window.requestAnimationFrame(resetView);
        close.focus();
    }

    function decorate(graph) {
        colorMessages(
            graph,
            controlFlowMessageLabels,
            controlFlowColor,
            "control-flow-yellow",
            true
        );
        colorMessages(
            graph,
            readinessEventLabels,
            readinessEventColor,
            "readiness-blue",
            true
        );
        colorMessages(
            graph,
            threadHandoffMessageLabels,
            threadHandoffColor,
            "thread-handoff-red",
            false,
            threadHandoffMessagePrefixes
        );
        colorThreadHandoffBoundary(graph);
        if (!findRenderedSvg(graph)) {
            return;
        }
        if (graph.dataset.vtbViewerReady === "true") {
            return;
        }
        graph.dataset.vtbViewerReady = "true";
        graph.classList.add("vtb-zoomable-diagram");
        graph.tabIndex = 0;
        graph.setAttribute("role", "button");
        graph.setAttribute("aria-label", "点击放大查看图表");

        const button = document.createElement("button");
        button.className = "vtb-diagram-open";
        button.type = "button";
        button.textContent = "⛶ 放大查看";
        button.addEventListener("click", function (event) {
            event.stopPropagation();
            openViewer(graph);
        });
        graph.prepend(button);

        graph.addEventListener("click", function (event) {
            if (event.target.closest("a")) {
                return;
            }
            openViewer(graph);
        });
        graph.addEventListener("keydown", function (event) {
            if (event.key === "Enter" || event.key === " ") {
                event.preventDefault();
                openViewer(graph);
            }
        });
    }

    function scan() {
        document.querySelectorAll(".mermaidgraph").forEach(decorate);
    }

    scan();
    document.addEventListener("DOMContentLoaded", scan);
    const observer = new MutationObserver(scan);
    observer.observe(document.documentElement, { childList: true, subtree: true });
}());
