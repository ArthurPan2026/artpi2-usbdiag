const {
  STAGES, verdictLabel, safeNumber, formatUs, formatHex, speedName, errorName,
  ruleDescription, findFailedTransfer, describeOutcome, requestName, responseName,
  drawHex, drawInspector, drawRuleOverview, drawDescriptorTree,
  readEvent,
} = await import("/assets/data.js");
const { lazyAsset } = await import("/assets/asset_loader.js");
const { createEvidencePoller, emptyEvidenceOutcome } =
  await import("/assets/evidence_poll.js");
const $ = id => document.getElementById(id);
const state = {
  manifest: null, snapshot: null, mode: "usb", connected: false,
  evidencePhase: "connecting",
  selectedStage: 0, selectedEvent: null, selectedRule: null,
  selectedNode: null, selectedDescriptor: null, selectedReference: null, hexOffset: 0,
  showSerial: false, byteCache: new Map(), byteTicket: 0, loadingKey: null,
  expandedNodes: new Set(),
  exporting: false, pdfGenerating: false, lastSync: null, buildInfo: null,
};
let optionalFeatures = null;
let optionalFeaturesLoading = null;
let topologyStarted = false;
let pdfActionLoading = false;
const loadOptionalFeaturesAsset = lazyAsset("/assets/optional_features.js");
function text(id, value) {
  const next = value == null ? "--" : String(value);
  if ($(id).textContent !== next) $(id).textContent = next;
}
function element(tag, className, label) {
  const value = document.createElement(tag);
  if (className) value.className = className;
  if (label != null) value.textContent = String(label);
  return value;
}
function focusButton(rootId, key, value) {
  for (const button of $(rootId).querySelectorAll("button")) {
    if (button.dataset[key] === String(value)) { button.focus(); return; }
  }
}
function ruleInfo(rule) { return ruleDescription(rule, state.snapshot?.ruleset_version); }
function events() { return state.snapshot?.raw_events || []; }
function rules() { return state.snapshot?.rule_results || []; }
function eventBySequence(sequence) { return events().find(item => item.sequence === sequence) || null; }
function currentEvent() { return eventBySequence(state.selectedEvent); }
function currentRule() { return rules().find(item => item.id === state.selectedRule) || null; }
function failedTransfer() { return findFailedTransfer(events()); }
function topFailedRule() {
  return rules().find(item => item.verdict === "FAIL" && item.severity === 2) ||
    rules().find(item => item.verdict === "FAIL") || null;
}
function stageItem(index) {
  const [kind] = STAGES[index];
  return state.snapshot?.timeline?.find(item => item.kind === kind) ||
    { kind, status: "WAITING", event_sequence: 0, timestamp_us: null };
}
function selectedReference() {
  return state.selectedReference || currentRule()?.evidence_refs?.[0] || null;
}
function setConnection(connected, message = "") {
  state.connected = connected;
  const pill = $("connection-pill");
  pill.className = `pill ${connected ? "pill-good" : ["connecting", "loading"].includes(state.evidencePhase) ? "pill-muted" : "pill-bad"}`;
  const label = connected ? "板端 Web 可达" :
    ["connecting", "loading"].includes(state.evidencePhase) ? "正在连接开发板" : "连接不稳定";
  if (pill.textContent !== label) pill.textContent = label;
  $("connection-warning").hidden = !message;
  text("connection-warning", message);
}
function outcome() { return describeOutcome(state.snapshot); }
function renderHero() {
  const snapshot = state.snapshot;
  const [heading, explanation, tone] = snapshot ? outcome() :
    emptyEvidenceOutcome(state.evidencePhase);
  const hero = document.querySelector(".hero");
  hero.classList.remove("state-good", "state-bad", "state-warn");
  hero.classList.add(`state-${tone}`);
  text("headline", heading);
  text("verdict-detail", explanation);
  const device = snapshot?.device || {};
  text("device-name", device.present ? (device.product || "未提供产品名称") : "尚无设备");
  text("device-ids", device.present ? `VID:PID ${formatHex(device.vid, 4)}:${formatHex(device.pid, 4)}` : "VID:PID --:--");
  text("device-speed", device.present ? speedName(device.speed) : "速度待确认");
  const hasSerial = Boolean(device.present && device.serial);
  $("serial-toggle").hidden = state.mode !== "engineer" || !hasSerial;
  text("serial-toggle", state.showSerial ? "隐藏" : "显示");
  text("device-serial", hasSerial ? (state.mode === "engineer" && state.showSerial ? device.serial : "已隐藏") : "未提供");
  const integrity = snapshot?.capture_integrity || "UNAVAILABLE";
  text("integrity-value", ({ COMPLETE: "完整", DEGRADED: "已降级", UNAVAILABLE: "不可用" })[integrity] || integrity);
  const summary = snapshot?.rule_summary || {};
  text("rule-counts", snapshot ? `${summary.passed || 0}/${summary.total || 12} ${snapshot.is_final && integrity === "COMPLETE" ? "通过" : "局部符合"} · ${summary.failed || 0} 失败` : "-- / 12");
  text("session-revision", snapshot ? `${snapshot.session_id} / ${snapshot.revision}` : "-- / --");
  const syncText = state.lastSync ?
    state.evidencePhase === "ready" ?
      `网页同步 ${state.lastSync.toLocaleTimeString("zh-CN")} · 时间线为 Host 软件观测时间` :
      `上次校验 ${state.lastSync.toLocaleTimeString("zh-CN")} · 显示上次已校验快照` :
    "Host 软件观测时间 · 暂无快照";
  text("last-sync", syncText);
  $("export-button").disabled = !snapshot || !state.connected || state.exporting;
  $("pdf-button").disabled = !snapshot || !state.connected || state.pdfGenerating;
}
function renderTimeline() {
  const list = $("timeline");
  const fragment = document.createDocumentFragment();
  STAGES.forEach(([kind, title], index) => {
    const item = stageItem(index);
    const status = item.status || (item.event_sequence ? "OBSERVED" : "WAITING");
    const li = element("li");
    const button = element("button", `stage-button is-${status.toLowerCase()}${state.selectedStage === index ? " is-selected" : ""}`);
    button.type = "button";
    button.dataset.stage = String(index);
    if (state.selectedStage === index) button.setAttribute("aria-current", "step");
    button.setAttribute("aria-label", `${index + 1}. ${title}，${status === "FAILED" ? "失败" : status === "OBSERVED" ? "已观察" : "等待"}`);
    button.append(element("span", "stage-index", String(index + 1)),
      element("span", "stage-label", title),
      element("span", "stage-status", status === "FAILED" ? "失败" : status === "OBSERVED" ? "已观察" : "等待"));
    button.addEventListener("click", () => selectStage(index));
    li.append(button);
    fragment.append(li);
  });
  list.replaceChildren(fragment);
  const count = state.snapshot?.timeline?.filter(item => item.status === "OBSERVED").length || 0;
  text("journey-caption", `${count}/8 步已观察 · 时间为 Host 软件观测时间`);
}
function renderLearn() {
  const [kind, title, explanation] = STAGES[state.selectedStage];
  const item = stageItem(state.selectedStage);
  const event = currentEvent();
  const rule = currentRule();
  text("step-title", `${state.selectedStage + 1}. ${title}`);
  text("step-explain", explanation);
  text("host-action", requestName(event));
  text("device-result", responseName(event));
  text("step-time", item.event_sequence ? `${formatUs(item.timestamp_us)} · Host 软件观测时间，不代表总线硬件时序` : "该步骤尚未观察到对应事件");
  if (rule) {
    const info = ruleInfo(rule);
    text("selected-rule-title", `${info.title} · ${verdictLabel(rule, state.snapshot)}`);
  text("rule-meaning", `${info.meaning}${rule.verdict === "PASS" && (!state.snapshot?.is_final || state.snapshot?.capture_integrity !== "COMPLETE") ? " 整体未判定。" : ""}`);
    text("rule-expected", rule.expected || "未记录");
    text("rule-observed", rule.observed || (rule.verdict === "NOT_OBSERVED" ? "尚无充分观测" : "未记录"));
    text("rule-clause", rule.clause || "未记录");
  } else if (event?.return_status < 0) {
    text("selected-rule-title", `Host 传输失败 · ${errorName(event.return_status)}`);
    text("rule-meaning", "此处记录 Host 返回状态；缺少设备响应字节时不能判定描述符字段违规。 ");
    text("rule-expected", "控制请求成功完成");
    text("rule-observed", `return_status=${event.return_status} · ${event.capture_status || "无捕获"}`);
    text("rule-clause", event.setup?.bRequest === 6 ? "USB 2.0 §9.4.3 GET_DESCRIPTOR" : "以实际请求和返回状态为准");
  } else {
    text("selected-rule-title", "此阶段暂无直接规则结论");
    text("rule-meaning", "先查看 Host 事件；只有规则引用了有效证据，才能给出本项判定。");
    text("rule-expected", "--"); text("rule-observed", "--"); text("rule-clause", "--");
  }
  const reference = selectedReference();
  text("proof-reference", reference ? (reference.byte_length ? `#${reference.event_sequence} +${reference.byte_offset}:${reference.byte_length}B` : `#${reference.event_sequence} 状态，无字节`) : event ? `#${event.sequence} 无引用` : "等待事件");
  $("jump-evidence").disabled = !event;
}
function renderRuleOverview() {
  drawRuleOverview($("rule-overview"), rules(), state.snapshot, state.selectedRule, selectRule);
}
function renderEvents() {
  optionalFeatures?.renderEvents();
}
function renderRules() {
  optionalFeatures?.renderRules();
}
function renderTree() {
  const source = state.snapshot?.descriptor_nodes || [];
  const search = $("search-input").value.trim().toLowerCase();
  const fieldKey = state.selectedDescriptor ?
    `${state.selectedDescriptor.id}:${state.selectedDescriptor.name}` : null;
  drawDescriptorTree($("descriptor-tree"), source, search, state.selectedNode,
    fieldKey, state.expandedNodes, selectDescriptor);
  text("node-count", `${source.length} 节点`);
}
function renderInspector() {
  drawInspector(currentEvent(), selectedReference(), currentRule(), state.snapshot,
    state.selectedDescriptor);
}
function renderHex(target, bytes, reference, start, count) {
  drawHex($(target), bytes, reference, start, count, state.selectedEvent);
}
function renderBytes() {
  const event = currentEvent();
  const reference = selectedReference();
  const bytes = event?.captured_length === 0 ? new Uint8Array() :
    event ? state.byteCache.get(`${state.manifest?.token}:${event.sequence}`) : null;
  if (!event) {
    text("proof-status", "没有可定位的原始事件。");
    renderHex("learn-hex", new Uint8Array(), null, 0, 64);
    renderHex("inspector-hex", new Uint8Array(), null, 0, 256);
    text("hex-window", "尚无字节");
    $("hex-prev").disabled = $("hex-next").disabled = true;
    return;
  }
  let warning = "";
  if (reference && reference.event_sequence !== event.sequence) warning = "规则引用的事件不在当前选择中。";
  else if (reference && reference.byte_length && reference.byte_offset + reference.byte_length > event.captured_length) warning = "引用越界，不予高亮。";
  else if (event.capture_status === "LOCAL_TRUNCATED") warning = "本机捕获已截断，请勿把截断解释为设备违规。";
  else if (event.captured_length === 0) warning = `无返回字节 · ${event.capture_status || "UNAVAILABLE"}`;
  else warning = `已捕获 ${event.captured_length} 字节 · CRC32 ${event.payload_crc32}`;
  text("proof-status", warning);
  const validReference = reference?.event_sequence === event.sequence &&
    reference.byte_offset >= 0 && reference.byte_offset + reference.byte_length <= event.captured_length ? reference : null;
  const learnOffset = validReference ? Math.floor(validReference.byte_offset / 16) * 16 : 0;
  if (bytes?.length && state.hexOffset >= bytes.length) state.hexOffset = Math.floor((bytes.length - 1) / 256) * 256;
  renderHex("learn-hex", bytes, validReference, learnOffset, 64);
  renderHex("inspector-hex", bytes, validReference, state.hexOffset, 256);
  const length = bytes?.length ?? event.captured_length;
  text("hex-window", length ? `偏移 ${state.hexOffset}–${Math.min(state.hexOffset + 255, length - 1)} / ${length} 字节` : "尚无字节");
  $("hex-prev").disabled = state.hexOffset === 0;
  $("hex-next").disabled = state.hexOffset + 256 >= length;
}
async function ensureSelectedBytes() {
  const event = currentEvent();
  if (!event || !state.manifest || !event.captured_length) return;
  const key = `${state.manifest.token}:${event.sequence}`;
  if (state.byteCache.has(key) || state.loadingKey === key) return;
  const ticket = ++state.byteTicket;
  state.loadingKey = key;
  try {
    const bytes = await readEvent(state.manifest, event);
    if (ticket !== state.byteTicket || key !== `${state.manifest?.token}:${state.selectedEvent}`) return;
    state.byteCache.set(key, bytes);
    renderBytes();
  } catch (error) {
    if (ticket !== state.byteTicket) return;
    text("proof-status", `原始字节读取失败：${error.message}`);
    if (error.status === 409) void pollLive(true);
  } finally {
    if (state.loadingKey === key) state.loadingKey = null;
  }
}
function ensureOptionalFeatures() {
  if (optionalFeatures) return Promise.resolve(optionalFeatures);
  if (optionalFeaturesLoading) return optionalFeaturesLoading;
  optionalFeaturesLoading = loadOptionalFeaturesAsset().then(module => {
    optionalFeatures = module.createOptionalFeatures({
      state, $, element, renderAll, renderHero, renderManifestDetails, setConnection,
      pollLive: force => pollLive(force), events, rules, ruleInfo,
      verdictLabel, requestName, formatUs, errorName, selectEvent, selectRule,
    });
    optionalFeatures.render();
    return optionalFeatures;
  }).finally(() => { optionalFeaturesLoading = null; });
  return optionalFeaturesLoading;
}
function refreshTopology(force = false) {
  if (document.hidden || state.mode !== "usb" || !state.snapshot) return Promise.resolve(false);
  return ensureOptionalFeatures().then(features => features.refreshTopology(force))
    .catch(error => {
      const status = $("usb-topology-status");
      status.hidden = false;
      status.className = "usb-topology-status is-warning";
      status.textContent = `拓扑暂不可用：${error.message}；证据继续，点击刷新拓扑重试。`;
      return false;
    });
}
function cancelTopologyRefresh(reason) {
  optionalFeatures?.cancelTopologyRefresh(reason);
}
function renderAll(includeTree = true) {
  renderHero(); renderTimeline(); renderLearn(); renderRuleOverview();
  renderEvents(); renderRules();
  if (includeTree) renderTree();
  renderInspector(); renderBytes();
  renderManifestDetails();
  optionalFeatures?.render();
  void ensureSelectedBytes();
}
function renderManifestDetails() {
  text("manifest-details", state.manifest ? JSON.stringify({
    token: state.manifest.token, snapshot_id: state.manifest.snapshot_id,
    revision: state.manifest.revision, total_bytes: state.manifest.total_bytes,
    json_crc32: state.manifest.json_crc32, content_crc32: state.manifest.content_crc32,
    ruleset_version: state.snapshot?.ruleset_version,
    build_source_fingerprint: state.buildInfo?.source_fingerprint,
  }, null, 2) : "等待 manifest");
}
function setSelectedEvent(sequence, reference = null) {
  state.byteTicket++;
  state.loadingKey = null;
  state.selectedEvent = sequence;
  state.selectedReference = reference;
  const event = eventBySequence(sequence);
  state.hexOffset = reference?.byte_length && event &&
    reference.byte_offset < event.captured_length ?
    Math.floor(reference.byte_offset / 256) * 256 : 0;
}
function selectStage(index) {
  state.selectedStage = index;
  state.selectedNode = null;
  state.selectedDescriptor = null;
  const event = eventBySequence(stageItem(index).event_sequence);
  const rule = event ? rules().find(item => (item.evidence_refs || []).some(ref => ref.event_sequence === event.sequence)) : null;
  state.selectedRule = rule?.id || null;
  setSelectedEvent(event?.sequence || null, rule?.evidence_refs?.[0] || null);
  renderAll();
  focusButton("timeline", "stage", index);
}
function selectRule(rule) {
  state.selectedRule = rule.id;
  state.selectedNode = null;
  state.selectedDescriptor = null;
  const reference = rule.evidence_refs?.[0] || null;
  const event = eventBySequence(reference?.event_sequence || rule.event_sequence);
  setSelectedEvent(event?.sequence || null, reference);
  const index = STAGES.findIndex((_, stage) => stageItem(stage).event_sequence === event?.sequence);
  if (index >= 0) state.selectedStage = index;
  renderAll();
  focusButton(state.mode === "learn" ? "rule-overview" : "rule-list", "rule", rule.id);
}
function selectEvent(event) {
  state.selectedRule = null;
  state.selectedNode = null;
  state.selectedDescriptor = null;
  setSelectedEvent(event.sequence);
  const index = STAGES.findIndex((_, stage) => stageItem(stage).event_sequence === event.sequence);
  if (index >= 0) state.selectedStage = index;
  renderAll();
  focusButton("event-list", "sequence", event.sequence);
}
function selectDescriptor(node, field, refreshTree = true) {
  state.selectedNode = node.id;
  state.selectedRule = null;
  state.selectedDescriptor = { id: node.id, type: node.type,
    title: node.title, name: field?.name, value: field?.value };
  const event = eventBySequence(node.event_sequence);
  const offset = safeNumber(node.byte_offset) + safeNumber(field?.byte_offset);
  const length = safeNumber(field?.byte_length ?? node.byte_length);
  setSelectedEvent(event?.sequence || null, { event_sequence: node.event_sequence, byte_offset: offset, byte_length: length, role: field?.name || node.type });
  const index = STAGES.findIndex((_, stage) => stageItem(stage).event_sequence === event?.sequence);
  if (index >= 0) state.selectedStage = index;
  renderAll(refreshTree);
  if (!refreshTree) {
    for (const summary of $("descriptor-tree").querySelectorAll("summary.is-selected")) summary.classList.remove("is-selected");
    for (const summary of $("descriptor-tree").querySelectorAll("summary")) {
      if (summary.dataset.node === String(node.id)) summary.classList.add("is-selected");
    }
  }
}
function chooseDefault(snapshot) {
  const failure = [...(snapshot.raw_events || [])].reverse().find(item => item.return_status < 0);
  const openingStage = snapshot.timeline?.find(item => item.kind === "DEVICE_8B" && item.event_sequence);
  let source = failure || eventBySequence(openingStage?.event_sequence);
  const rule = topFailedRule() || rules().find(item => item.verdict === "PASS" &&
    item.evidence_refs?.some(ref => ref.event_sequence === source?.sequence)) ||
    rules().find(item => item.verdict === "PASS" &&
      item.evidence_refs?.some(ref => eventBySequence(ref.event_sequence)));
  state.selectedRule = rule?.id || null;
  const reference = rule?.evidence_refs?.[0] || null;
  source = eventBySequence(reference?.event_sequence) || source;
  const preferred = snapshot.timeline?.find(item => item.event_sequence === source?.sequence) ||
    (failure ? snapshot.timeline?.find(item => item.status === "FAILED") : openingStage);
  const chosen = STAGES.findIndex(([kind]) => kind === preferred?.kind);
  state.selectedStage = chosen >= 0 ? chosen : 0;
  setSelectedEvent(source?.sequence || null, reference);
}
function setMode(mode) {
  state.mode = mode;
  $("usb-view").hidden = mode !== "usb";
  $("learn-view").hidden = mode !== "learn";
  $("engineer-view").hidden = mode !== "engineer";
  for (const [name, id] of [["usb", "mode-usb"], ["engineer", "mode-engineer"], ["learn", "mode-learn"]]) {
    const active = name === mode;
    $(id).classList.toggle("is-active", active);
    $(id).setAttribute("aria-pressed", String(active));
  }
  if (mode === "usb") {
    if (state.snapshot) {
      void refreshTopology(true);
    }
  } else {
    cancelTopologyRefresh("view-hidden");
  }
  if (mode === "learn" || mode === "engineer") {
    void ensureOptionalFeatures().then(features =>
      mode === "learn" ? features.ensureTasks() : features.ensureLists()).catch(error => {
        const target = mode === "learn" ? "task-list" : "event-list";
        $(target).replaceChildren(element("p", "small-note",
          `辅助模块失败：${error.message}；重进可重试。`));
      });
  }
  renderHero();
}
const evidencePoller = createEvidencePoller({
  state,
  renderAll,
  eventBySequence,
  rules,
  currentRule,
  chooseDefault,
  onStatus: status => {
    state.evidencePhase = status.phase;
    if (status.lastSync) state.lastSync = status.lastSync;
    setConnection(status.connected, status.message || "");
    renderHero();
    if (status.phase === "ready" && state.mode === "usb" && !topologyStarted) {
      topologyStarted = true;
      void refreshTopology(true);
    }
  },
});
const pollLive = force => evidencePoller.refresh(force);
$("mode-usb").addEventListener("click", () => setMode("usb"));
$("mode-learn").addEventListener("click", () => setMode("learn"));
$("mode-engineer").addEventListener("click", () => setMode("engineer"));
$("refresh-button").addEventListener("click", () => void pollLive(true));
$("usb-refresh-button").addEventListener("click", () => {
  if (!optionalFeatures) loadOptionalFeaturesAsset.reset();
  void refreshTopology(true);
});
$("export-button").addEventListener("click", () => {
  if (!state.snapshot || !state.manifest || state.exporting) return;
  void ensureOptionalFeatures().then(features => features.exportEvidence()).catch(error =>
    setConnection(state.connected, `导出模块暂不可用：${error.message}`));
});
$("pdf-button").addEventListener("click", () => {
  if (!state.snapshot || !state.manifest || state.pdfGenerating || pdfActionLoading) return;
  const requestedSnapshot = state.snapshot;
  const requestedManifest = state.manifest;
  pdfActionLoading = true;
  state.pdfGenerating = true;
  renderHero();
  const progress = $("pdf-progress");
  progress.hidden = false;
  progress.textContent = "正在准备 PDF 预览…";
  void ensureOptionalFeatures().then(features =>
    features.generatePdf(requestedSnapshot, requestedManifest)).catch(error => {
    progress.textContent = `PDF 失败：PDF 模块准备失败（${error.name || "错误"}）：${error.message || "未知错误"}。页面仍可用，可再次点击生成。`;
  }).finally(() => {
    pdfActionLoading = false;
    state.pdfGenerating = false;
    renderHero();
  });
});
$("task-list").addEventListener("usbdiag-task-locate", event => {
  selectRule(event.detail.rule);
});
$("serial-toggle").addEventListener("click", () => { state.showSerial = !state.showSerial; renderHero(); });
$("jump-evidence").addEventListener("click", () => {
  setMode("engineer");
  $("inspector-title").scrollIntoView({ block: "center", behavior: "smooth" });
});
for (const id of ["search-input", "event-filter", "rule-filter"]) {
  $(id).addEventListener(id === "search-input" ? "input" : "change", () => {
    renderEvents(); renderRules(); renderTree();
  });
}
$("hex-prev").addEventListener("click", () => { state.hexOffset = Math.max(0, state.hexOffset - 256); renderBytes(); });
$("hex-next").addEventListener("click", () => { state.hexOffset += 256; renderBytes(); });
$("timeline").addEventListener("keydown", event => {
  if (event.key !== "ArrowRight" && event.key !== "ArrowLeft") return;
  event.preventDefault();
  const change = event.key === "ArrowRight" ? 1 : -1;
  const index = Math.max(0, Math.min(7, state.selectedStage + change));
  selectStage(index);
});
void evidencePoller.start();
setMode("usb");
document.addEventListener("visibilitychange", () => {
  if (document.hidden) {
    evidencePoller.pause();
    cancelTopologyRefresh("hidden");
    return;
  }
  void evidencePoller.resume().then(() => {
    if (state.mode === "usb" && state.snapshot) {
      void refreshTopology(true);
    }
  });
});
window.addEventListener("pagehide", () => {
  evidencePoller.pause();
  cancelTopologyRefresh("pagehide");
});
window.addEventListener("pageshow", event => {
  if (!event.persisted) return;
  void evidencePoller.resume().then(() => {
    if (state.mode === "usb" && state.snapshot) void refreshTopology(true);
  });
});
