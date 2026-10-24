import { lazyAsset } from "/assets/asset_loader.js";
import { readEvent, downloadEvidence } from "/assets/data.js";

const loadTasks = lazyAsset("/assets/tasks.js");
const loadBuildInfo = lazyAsset("/assets/build_info.js");
const loadTopology = lazyAsset("/assets/topology.js");
const loadHistory = lazyAsset("/assets/history.js");
const loadLists = lazyAsset("/assets/lists.js");
const loadPdfAsset = lazyAsset("/assets/report_pdf.js");
const TOPOLOGY_POLL_MS = 10000;

export function createOptionalFeatures(options) {
  const { state, $, element, renderAll, renderHero, renderManifestDetails,
    setConnection, pollLive } = options;
  let tasksRenderer = null;
  let tasksLoading = null;
  let buildInfoLoading = null;
  let buildInfoError = null;
  let topologyModule = null;
  let listsModule = null;
  let listsLoading = null;
  let historyBound = false;
  let historyLoading = false;
  let topologyPollTimer = null;
  let topologyRetryPaused = false;

  async function loadPdfModule() {
    try {
      return await loadPdfAsset();
    } catch (error) {
      if (error.name !== "TypeError") throw error;
      loadPdfAsset.reset();
      await new Promise(resolve => setTimeout(resolve, 250));
      return loadPdfAsset();
    }
  }

  function scheduleTopologyPoll() {
    clearTimeout(topologyPollTimer);
    if (topologyRetryPaused || document.hidden || state.mode !== "usb" || !state.snapshot) return;
    topologyPollTimer = setTimeout(() => {
      topologyPollTimer = null;
      void refreshTopology();
    }, TOPOLOGY_POLL_MS);
  }

  function renderBuildInfo() {
    const target = $("build-info");
    if (state.buildInfo) {
      target.textContent = `源码指纹 ${state.buildInfo.source_fingerprint} · ${state.buildInfo.profile}/${state.buildInfo.build_type} · RT-Thread ${state.buildInfo.rt_thread} · LVGL ${state.buildInfo.lvgl}`;
      return;
    }
    if (!buildInfoError) {
      target.textContent = "生成 PDF 时读取构建身份；这不会阻塞页面和 USB 证据显示。";
      return;
    }
    target.replaceChildren(document.createTextNode(`构建身份暂不可用：${buildInfoError.message}。`));
    const retry = element("button", "text-button", "重试");
    retry.addEventListener("click", () => {
      buildInfoError = null;
      loadBuildInfo.reset();
        void ensureBuildInfo().catch(() => {});
    });
    target.append(" ", retry);
  }

  async function ensureBuildInfo() {
    if (!state.snapshot || state.buildInfo || buildInfoLoading) return buildInfoLoading;
    buildInfoError = null;
    buildInfoLoading = (async () => {
      try {
        state.buildInfo = await readBuildInfo();
        renderManifestDetails?.();
        renderBuildInfo();
      } catch (error) {
        buildInfoError = error;
        renderBuildInfo();
        throw error;
      } finally {
        buildInfoLoading = null;
      }
    })();
    return buildInfoLoading;
  }

  async function readBuildInfo() {
    try {
      const module = await loadBuildInfo();
      return await module.readBuildInfo();
    } catch (error) {
      if (error.name !== "TypeError") throw error;
      loadBuildInfo.reset();
      await new Promise(resolve => setTimeout(resolve, 250));
      const module = await loadBuildInfo();
      return module.readBuildInfo();
    }
  }

  async function generatePdf(snapshot, manifest) {
    const requestedSnapshot = snapshot || state.snapshot;
    const requestedManifest = manifest || state.manifest;
    if (!requestedSnapshot || !requestedManifest) return;
    const progress = $("pdf-progress");
    progress.hidden = false;
    progress.textContent = "正在核对构建身份并准备 PDF 预览…";
    let currentStage = "读取构建身份";
    try {
      await ensureBuildInfo();
      if (!state.buildInfo) throw new Error("构建身份暂不可用");
      currentStage = "加载 PDF 组件";
      const { openVerificationPdf } = await loadPdfModule();
      const result = await openVerificationPdf(
        requestedSnapshot, requestedManifest, state.buildInfo, message => {
          if (message.startsWith("开发板正在冻结证据")) currentStage = "启动板端 PDF 任务";
          else if (message.includes("正在板端排版") || message.includes("正在生成报告")) {
            currentStage = "等待板端排版";
          } else if (message.startsWith("下载板端 PDF") || message.includes("重新接收同一份")) {
            currentStage = "接收 PDF 正文";
          } else if (message.includes("正在校验内容")) currentStage = "校验 PDF 正文";
          else if (message.includes("正在打开新标签页")) currentStage = "打开 PDF 预览";
          progress.textContent = message;
        });
      const elapsed = Number.isFinite(result.elapsedMs) ?
        ` · ${Math.max(0, result.elapsedMs / 1000).toFixed(1)} 秒` : "";
      progress.replaceChildren(document.createTextNode(result.previewOpened ?
        `PDF 已在新标签页打开，可从浏览器工具栏下载或打印${elapsed}。` :
        `PDF 已校验；浏览器拦截了自动打开，请点击下方链接预览${elapsed}：`));
      if (result.fallbackUrl) {
        const link = element("a", "pdf-preview-link", result.fileName || "打开 PDF 预览");
        link.href = result.fallbackUrl;
        link.target = "_blank";
        link.rel = "noopener";
        progress.append(" ", link);
      }
    } catch (error) {
      loadPdfAsset.reset();
      const detail = error.pdfStage ? error.message :
        `${currentStage}失败（${error.name || "错误"}）：${error.message || "未知错误"}`;
      progress.textContent = `PDF 失败：${detail}。页面仍可用，可再次点击生成。`;
    }
  }

  async function ensureTasks() {
    if (tasksRenderer) return tasksRenderer;
    if (tasksLoading) return tasksLoading;
    $("task-list").replaceChildren(element("p", "small-note", "正在加载教学核验任务…"));
    tasksLoading = loadTasks().then(module => {
      tasksRenderer = module.renderTeachingTasks;
      renderAll(false);
      return tasksRenderer;
    }).catch(error => {
      $("task-list").replaceChildren(element("p", "small-note",
        `教学任务模块暂不可用：${error.message}。重新进入教学导览可重试。`));
      loadTasks.reset();
      tasksLoading = null;
      return null;
    });
    return tasksLoading;
  }

  async function ensureLists() {
    if (listsModule) return listsModule;
    if (listsLoading) return listsLoading;
    listsLoading = loadLists().then(module => {
      listsModule = module;
      renderAll(false);
      return module;
    }).catch(error => {
      const message = element("p", "small-note", `研发列表模块暂不可用：${error.message}。重新进入研发核验可重试。`);
      $("event-list").replaceChildren(message.cloneNode(true));
      $("rule-list").replaceChildren(message);
      loadLists.reset();
      listsLoading = null;
      return null;
    });
    return listsLoading;
  }

  function renderEvents() {
    if (!listsModule) return;
    const search = $("search-input").value.trim().toLowerCase();
    $("event-count").textContent = listsModule.renderEvents($("event-list"), options.events(),
      state.selectedEvent, search, $("event-filter").value, options.requestName,
      options.formatUs, options.errorName, options.selectEvent);
  }

  function renderRules() {
    if (!listsModule) return;
    const search = $("search-input").value.trim().toLowerCase();
    $("rules-count").textContent = listsModule.renderRules($("rule-list"), options.rules(),
      state.selectedRule, search, $("rule-filter").value, options.ruleInfo,
      options.verdictLabel, state.snapshot, options.selectRule);
  }

  function renderTasks() {
    if (tasksRenderer) {
      tasksRenderer($("task-list"), state.snapshot, state.manifest?.token || null,
        async event => {
          if (!state.manifest) throw new Error("快照代号不可用");
          const token = state.manifest.token;
          const bytes = await readEvent(state.manifest, event);
          if (token !== state.manifest?.token) throw new Error("核验期间快照代号已变化");
          state.byteCache.set(`${token}:${event.sequence}`, bytes);
          return bytes;
        });
    } else if (state.mode === "learn") void ensureTasks();
  }

  async function refreshTopology(force = false) {
    if (document.hidden || state.mode !== "usb") return false;
    if (force) topologyRetryPaused = false;
    const status = $("usb-topology-status");
    try {
      if (!topologyModule) {
        if (force) loadTopology.reset();
        status.hidden = false;
        status.className = "usb-topology-status";
        status.textContent = "正在加载 USB 拓扑模块…";
        topologyModule = await loadTopology();
      }
      if (document.hidden || state.mode !== "usb") return false;
      const ok = await topologyModule.refreshTopology(force);
      topologyRetryPaused = !ok;
      if (ok) scheduleTopologyPoll();
      return ok;
    } catch (error) {
      topologyModule = null;
      topologyRetryPaused = true;
      status.hidden = false;
      status.className = "usb-topology-status is-warning";
      status.textContent = error.message.includes("自动重试已暂停") ?
        "拓扑模块加载已暂停；点击刷新拓扑继续。" :
        `拓扑暂不可用：${error.message}；证据继续，点击刷新拓扑重试。`;
      return false;
    }
  }

  function cancelTopologyRefresh(reason) {
    clearTimeout(topologyPollTimer);
    topologyPollTimer = null;
    topologyModule?.cancelTopologyRefresh(reason);
  }

  function bindHistory() {
    const panel = $("history-panel");
    panel.addEventListener("toggle", () => {
      if (!panel.open || historyBound || historyLoading) return;
      historyLoading = true;
      void loadHistory().then(module => {
        module.bindHistoryPanel();
        historyBound = true;
        panel.dispatchEvent(new Event("toggle"));
      }).catch(error => {
        $("history-list").replaceChildren(element("p", "small-note",
          `历史摘要模块暂不可用：${error.message}；关闭后重新打开可重试。`));
        loadHistory.reset();
      }).finally(() => { historyLoading = false; });
    });

  }

  async function exportEvidence() {
    if (!state.snapshot || !state.manifest || state.exporting) return;
    state.exporting = true;
    renderHero();
    try {
      await downloadEvidence(state.manifest, state.snapshot);
    } catch (error) {
      setConnection(state.connected, `导出失败：${error.message}`);
      if (error.status === 409) void pollLive(true);
    } finally {
      state.exporting = false;
      renderHero();
    }
  }

  bindHistory();
  return {
    ensureBuildInfo, ensureLists, ensureTasks, renderBuildInfo, renderEvents,
    renderRules, renderTasks, refreshTopology, cancelTopologyRefresh, exportEvidence,
    render() {
      renderBuildInfo();
      renderTasks();
    },
    generatePdf,
  };
}
