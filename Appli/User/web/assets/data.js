export const STAGES = [
  ["PORT_RESET", "端口复位", "Host 观察到设备连接后复位端口，为读取描述符建立初始状态。"],
  ["DEVICE_8B", "先读 8 字节", "Host 先读取设备描述符开头，确认后续控制传输所需的信息。"],
  ["SET_ADDRESS", "设置地址", "Host 分配设备地址，后续请求将指向这个地址。"],
  ["DEVICE_18B", "完整设备描述符", "Host 读取完整设备描述符，以确认设备身份和关键字段。"],
  ["CONFIGURATION", "配置与接口", "Host 读取配置树，核对配置、接口和端点的声明。"],
  ["STRINGS", "字符串信息", "Host 尝试读取厂商、产品等字符串；是否取得需看实际事件。"],
  ["SET_CONFIGURATION", "选择配置", "Host 请求启用选定配置，设备才能进入相应工作状态。"],
  ["ENUM_COMPLETE", "枚举结论", "综合已观察到的传输和规则，确定本次会话结果。"],
];
const RULE_BOOK_V2 = {
  DEV_B_LENGTH: ["设备描述符长度", "检查设备描述符声明的长度。"],
  DEV_TYPE: ["设备描述符类型", "确认请求得到的是 Device Descriptor。"],
  DEV_8B_PREFIX: ["前缀一致性", "前 8 字节应与完整描述符的前缀相符。"],
  DEV_FIELDS: ["EP0 包大小", "只核对设备描述符 bMaxPacketSize0 是否符合已观察到的连接速度。"],
  CFG_B_LENGTH: ["配置描述符头", "检查 bLength=9、bDescriptorType=2；必要时也核对返回头是否完整。"],
  CFG_TOTAL_LENGTH: ["配置总长度", "完整请求需返回恰好 wTotalLength 字节；短读不会误报为通过。"],
  CFG_TREE_WALK: ["描述符边界", "确认配置树的每段描述符均可完整遍历。"],
  CFG_INTERFACE_COUNT: ["接口数量", "对照声明接口数与实际唯一接口。"],
  IF_ENDPOINT_COUNT: ["端点数量", "逐个核对接口备用设置的端点数。"],
  EP_PACKET_SIZE: ["端点包大小", "按当前速度和端点类型检查包大小。"],
  EP_INTERVAL: ["端点间隔", "按当前速度和端点类型检查轮询间隔。"],
  EP0_STALL: ["必要请求结果", "检查枚举所需控制请求是否被 STALL。"],
};
const RULE_BOOK_V3 = {
  ...RULE_BOOK_V2,
  EP0_STALL: ["必要枚举控制请求", "Device/Configuration 描述符读取、SET_ADDRESS 与 SET_CONFIGURATION 均需成功完成；可选字符串请求的 STALL 不计为违规。必要请求未观察到或出现其他传输错误时只标记证据不足。"],
};
export const VERDICT = { PASS: "通过", FAIL: "失败", NOT_OBSERVED: "未观察", NOT_APPLICABLE: "不适用" };
export function verdictLabel(rule, snapshot) {
  if (rule?.verdict === "PASS" &&
      (!snapshot?.is_final || snapshot?.capture_integrity !== "COMPLETE")) {
    return "局部符合 · 整体不可判定";
  }
  return VERDICT[rule?.verdict] || rule?.verdict || "未知";
}

export function safeNumber(value) { return Number.isFinite(Number(value)) ? Number(value) : 0; }
export function formatUs(value) {
  return value != null && Number.isFinite(Number(value)) ? `+${(Number(value) / 1000).toFixed(2)} ms` : "时间未记录";
}
export function formatHex(value, width = 2) {
  return `0x${safeNumber(value).toString(16).toUpperCase().padStart(width, "0")}`;
}
export function speedName(speed) { return ({ 1: "低速", 2: "全速", 3: "高速" })[speed] || "速度未确认"; }
export function errorName(status) {
  return ({ "-14": "USB_ERR_TIMEOUT", "-8": "USB_ERR_STALL", "-10": "USB_ERR_NAK" })[status] || `返回状态 ${status}`;
}
export function ruleDescription(rule, rulesetVersion) {
  const book = rulesetVersion === 2 ? RULE_BOOK_V2 :
    rulesetVersion === 3 ? RULE_BOOK_V3 : null;
  if (book && book[rule?.id]) {
    const [title, meaning] = book[rule.id];
    return { title, meaning };
  }
  return { title: rule?.id || "尚无规则", meaning: "解释版本未知，请按板端期望、实测和规范条款复核。" };
}
export function findFailedTransfer(events) {
  return [...events].reverse().find(item => item.type === "CONTROL_TRANSFER" && item.return_status < 0) || null;
}
export function describeOutcome(snapshot) {
  if (!snapshot) return ["正在读取板端证据", "等待开发板发布当前 USB Host 快照。", "warn"];
  if (snapshot.capture_integrity !== "COMPLETE") {
    return ["证据不完整 · 不判定通过",
      `本轮丢弃事件 ${snapshot.session_dropped_event_count ?? snapshot.dropped_event_count ?? 0} · 丢弃字节 ${snapshot.session_dropped_payload_bytes ?? snapshot.dropped_payload_bytes ?? 0} · 原始溢出 ${snapshot.raw_event_overflow_count || 0}。`, "bad"];
  }
  const failed = findFailedTransfer(snapshot.raw_events || []);
  if (snapshot.session_state === "FAILED" && failed) {
    return [`枚举失败 · ${errorName(failed.return_status)}`,
      `Host 控制传输事件 #${failed.sequence} 失败。可查看请求字段及实际捕获状态，不能据此推断设备硬件根因。`, "bad"];
  }
  const rules = snapshot.rule_results || [];
  const rule = rules.find(item => item.verdict === "FAIL" && item.severity === 2) ||
    rules.find(item => item.verdict === "FAIL");
  if (rule) return [`规则失败 · ${ruleDescription(rule, snapshot.ruleset_version).title}`,
    `期望：${rule.expected || "未记录"}；实测：${rule.observed || "未记录"}。`, "bad"];
  if (!snapshot.is_final || (snapshot.rule_summary?.not_observed || 0) > 0) {
    return ["证据仍在收集", "尚有规则未观察到必要数据，当前不能形成整体通过结论。", "warn"];
  }
  if ((snapshot.rule_summary?.passed || 0) > 0) {
    return ["枚举完成 · 规则通过", "点击流程和规则，可查看每项结论所依据的 Host 观测与原始字节。", "good"];
  }
  return ["没有适用的规则结论", "可在研发核验中查看本次事件与规则状态。", "warn"];
}
export function requestName(event) {
  if (!event) return "尚未取得对应 Host 事件";
  if (event.type !== "CONTROL_TRANSFER" || !event.has_setup) {
    return ({ PORT_RESET: "端口复位", ENUMERATION_COMPLETE: "枚举结束", DISCONNECT: "设备断开" })[event.type] || event.type;
  }
  const setup = event.setup || {};
  const names = { 5: "SET_ADDRESS", 6: "GET_DESCRIPTOR", 9: "SET_CONFIGURATION", 10: "GET_INTERFACE" };
  const descriptor = setup.bRequest === 6 ? ({ 1: "Device", 2: "Configuration", 3: "String", 0x22: "HID Report" })[setup.wValue >>> 8] : null;
  return `${names[setup.bRequest] || `bRequest ${formatHex(setup.bRequest)}`}${descriptor ? ` · ${descriptor}` : ""} · 请求 ${setup.wLength || 0} 字节`;
}
export function responseName(event) {
  if (!event) return "尚无可定位的响应";
  if (event.type === "PORT_RESET") return "端口复位已观察";
  if (event.return_status < 0) return `${errorName(event.return_status)} · ${event.capture_status || "无返回数据"}`;
  if (event.captured_length > 0) return `实际传输 ${event.transfer_length} 字节 · 捕获 ${event.captured_length} 字节`;
  return event.capture_status === "NO_DATA_STAGE" ? "请求完成 · 无数据阶段" : "请求完成 · 未捕获返回字节";
}

function node(tag, className, label) {
  const item = document.createElement(tag);
  if (className) item.className = className;
  if (label != null) item.textContent = String(label);
  return item;
}
export function drawHex(target, bytes, reference, start, count, selectedSequence) {
  const fragment = document.createDocumentFragment();
  if (!bytes || bytes.length === 0) {
    target.replaceChildren(node("p", "small-note", bytes ? "本事件无已捕获的返回字节。" : "正在按代号读取原始字节…"));
    return;
  }
  const end = Math.min(bytes.length, start + count);
  for (let rowStart = start; rowStart < end; rowStart += 16) {
    const line = node("div", "hex-line");
    line.append(node("span", "hex-offset", rowStart.toString(16).toUpperCase().padStart(4, "0")));
    let ascii = "";
    for (let index = rowStart; index < Math.min(rowStart + 16, end); index++) {
      const hit = reference && selectedSequence === reference.event_sequence &&
        index >= reference.byte_offset && index < reference.byte_offset + reference.byte_length;
      line.append(node("span", `hex-byte${hit ? " hit" : ""}`, bytes[index].toString(16).toUpperCase().padStart(2, "0")));
      ascii += bytes[index] >= 32 && bytes[index] <= 126 ? String.fromCharCode(bytes[index]) : ".";
    }
    line.append(node("span", "hex-ascii", ascii));
    fragment.append(line);
  }
  target.replaceChildren(fragment);
}
export function drawInspector(event, reference, rule, snapshot, descriptorSelection) {
  const fields = document.getElementById("request-fields");
  fields.replaceChildren();
  const rulePanel = document.getElementById("inspector-rule");
  rulePanel.replaceChildren();
  if (rule) {
    const info = ruleDescription(rule, snapshot?.ruleset_version);
      rulePanel.append(node("strong", "", `${info.title} · ${verdictLabel(rule, snapshot)}`),
      node("p", "", `期望：${rule.expected || "未记录"}`),
      node("p", "", `实测：${rule.observed || "未记录"}`),
      node("p", "", `条款：${rule.clause || "未记录"} · 原因码：${rule.reason_code || "未记录"}`),
      Number(rule.omitted_failure_reference_count || 0) > 0 ?
        node("p", "", `还有 ${rule.omitted_failure_reference_count} 个失败位置未展开（每条最多显示 4 处）。`) :
        node("p", "", "失败引用未超过展示容量。"));
  } else if (descriptorSelection) {
    rulePanel.append(node("strong", "", `描述符字段 · ${descriptorSelection.name || descriptorSelection.type}`),
      node("p", "", `解析值：${descriptorSelection.value || descriptorSelection.title || "未解析"}`),
      node("p", "", `节点 #${descriptorSelection.id} · 来源事件 #${reference?.event_sequence ?? "--"}`));
  }
  document.getElementById("inspector-title").textContent = event ?
    `事件 #${event.sequence} · ${requestName(event)}` : "选择一条事件";
  document.getElementById("inspector-note").textContent = event ?
    `尝试 ${event.attempt_id ?? "--"} · 端口 ${event.port_valid ? event.port : "未知"} · ${formatUs(event.timestamp_us)}（Host 软件观测）` :
    "规则、描述符和字节引用会联动定位到这里。";
  function addField(name, value) {
    const row = node("div");
    row.append(node("dt", "", name), node("dd", "", value));
    fields.append(row);
  }
  if (event) {
    addField("类型", event.type || "--");
    addField("返回状态", event.return_status < 0 ? errorName(event.return_status) : String(event.return_status));
    if (event.has_setup) {
      for (const key of ["bmRequestType", "bRequest", "wValue", "wIndex", "wLength"]) {
        addField(key, formatHex(event.setup?.[key], key.startsWith("w") ? 4 : 2));
      }
    }
    addField("实际传输 / 已捕获", `${event.transfer_length_known ? event.transfer_length : "未知"} / ${event.captured_length} 字节`);
    addField("捕获状态", event.capture_status || "--");
    addField("Host 软件传输耗时", event.timing_valid ? `${event.duration_us} μs` : "未成对采集");
  }
  document.getElementById("inspector-reference").textContent = reference ?
    (reference.byte_length ?
      `事件 #${reference.event_sequence} · 偏移 ${reference.byte_offset} · 长度 ${reference.byte_length} · ${reference.role || "证据引用"}` :
      `事件 #${reference.event_sequence} · 状态观测，无原始字节范围 · ${reference.role || "观察引用"}`) :
    "当前没有字节引用";
}
export function drawRuleOverview(target, rules, snapshot, selectedRule, onSelect) {
  const fragment = document.createDocumentFragment();
  for (const rule of rules) {
    const info = ruleDescription(rule, snapshot?.ruleset_version);
    const button = node("button", `rule-chip${selectedRule === rule.id ? " is-selected" : ""}`);
    button.type = "button";
    button.dataset.rule = rule.id;
    const tone = rule.verdict === "PASS" && snapshot?.capture_integrity === "COMPLETE" && snapshot?.is_final ? "pass" :
      rule.verdict === "FAIL" ? "fail" : rule.verdict === "NOT_APPLICABLE" ? "na" : "missing";
    button.append(node("strong", "", info.title),
      node("span", `verdict-${tone}`, `${verdictLabel(rule, snapshot)} · ${rule.id}`));
    button.addEventListener("click", () => onSelect(rule));
    fragment.append(button);
  }
  target.replaceChildren(fragment);
}
export function drawDescriptorTree(target, source, search, selectedNode, selectedFieldKey, expandedNodes, onSelect) {
  const byId = new Map(source.map(item => [item.id, item]));
  const visible = new Set();
  for (const item of source) {
    const matches = `${item.title} ${item.type} ${(item.fields || []).map(field => `${field.name} ${field.value}`).join(" ")}`.toLowerCase().includes(search);
    if (!matches) continue;
    let parent = item;
    for (let guard = 0; parent && guard < source.length; guard++) {
      visible.add(parent.id);
      parent = byId.get(parent.parent_id);
    }
  }
  const fragment = document.createDocumentFragment();
  const visited = new Set();
  function addNode(item, destination, depth) {
    if (visited.has(item.id) || depth > 8 || (search && !visible.has(item.id))) return;
    visited.add(item.id);
    const detail = node("details", "tree-group");
    detail.open = search ? true : expandedNodes.has(item.id) || (depth === 0 && !expandedNodes.has(`closed:${item.id}`));
    detail.addEventListener("toggle", () => {
      if (detail.open) { expandedNodes.add(item.id); expandedNodes.delete(`closed:${item.id}`); }
      else { expandedNodes.delete(item.id); expandedNodes.add(`closed:${item.id}`); }
    });
    const summary = node("summary", selectedNode === item.id ? "is-selected" : "",
      `#${item.id} · ${item.type || "描述符"} · ${item.title || "字段未解析"}`);
    summary.dataset.node = String(item.id);
    summary.addEventListener("click", () => onSelect(item, null, false));
    detail.append(summary);
    for (const field of item.fields || []) {
      if (search && !`${field.name} ${field.value}`.toLowerCase().includes(search) &&
          !`${item.title} ${item.type}`.toLowerCase().includes(search)) continue;
      const fieldKey = `${item.id}:${field.name}`;
      const button = node("button", `tree-field${selectedFieldKey === fieldKey ? " is-selected" : ""}`, `${field.name} = ${field.value}`);
      button.type = "button";
      button.setAttribute("aria-pressed", String(selectedFieldKey === fieldKey));
      button.addEventListener("click", () => onSelect(item, field, false));
      detail.append(button);
    }
    for (const child of source.filter(candidate => candidate.parent_id === item.id)) {
      addNode(child, detail, depth + 1);
    }
    destination.append(detail);
  }
  for (const item of source.filter(value => !byId.has(value.parent_id))) addNode(item, fragment, 0);
  for (const item of source) addNode(item, fragment, 0);
  if (!source.length) fragment.append(node("p", "small-note", "尚无可解析的描述符节点。"));
  else if (search && !visible.size) fragment.append(node("p", "small-note", "没有匹配的描述符字段。"));
  target.replaceChildren(fragment);
}

const MAX_SUMMARY_BYTES = 131072;
const MAX_EVENT_BYTES = 8192;
const MAX_EVENTS = 64;
const REQUEST_TIMEOUT_MS = 3000;
export const SNAPSHOT_READ_TIMEOUT_MS = 20000;

function httpError(status, message, retryAfterMs = null) {
  const error = new Error(message || `HTTP ${status}`);
  error.status = status;
  if (retryAfterMs != null) error.retryAfterMs = retryAfterMs;
  return error;
}

function retryAfterMilliseconds(response) {
  const value = response.headers.get("Retry-After");
  if (!value) return null;
  const seconds = Number(value);
  if (Number.isFinite(seconds) && seconds >= 0) return Math.round(seconds * 1000);
  const date = Date.parse(value);
  return Number.isFinite(date) ? Math.max(0, date - Date.now()) : null;
}

async function fetchBody(path, signal, readBody, timeoutMs = REQUEST_TIMEOUT_MS) {
  if (signal?.aborted) {
    const error = new Error("请求已取消");
    error.name = "AbortError";
    error.cancelled = true;
    throw error;
  }
  const controller = new AbortController();
  let timedOut = false;
  const startedAt = performance.now();
  const abortFromParent = () => controller.abort();
  signal?.addEventListener("abort", abortFromParent, { once: true });
  const timeout = setTimeout(() => {
    timedOut = true;
    controller.abort();
  }, timeoutMs);
  try {
    const result = await fetch(path, { cache: "no-store", signal: controller.signal });
    if (result.status === 409) throw httpError(409, "快照已更新，正在重新读取");
    if (result.status === 416) throw httpError(416, "请求的原始字节范围无效");
    if (result.status === 503 || result.status === 429) {
      throw httpError(result.status, "开发板暂时忙，请稍后重试",
        retryAfterMilliseconds(result));
    }
    if (!result.ok) throw httpError(result.status);
    return await readBody(result);
  } catch (error) {
    if (timedOut) {
      const timeoutError = httpError(408,
        `请求超过 ${Math.ceil(timeoutMs / 1000)} 秒仍未完成`);
      timeoutError.timedOut = true;
      timeoutError.timeoutMs = timeoutMs;
      timeoutError.elapsedMs = Math.round(performance.now() - startedAt);
      timeoutError.requestPath = path;
      throw timeoutError;
    }
    if (signal?.aborted) {
      const cancelled = new Error("请求已取消");
      cancelled.name = "AbortError";
      cancelled.cancelled = true;
      throw cancelled;
    }
    throw error;
  } finally {
    clearTimeout(timeout);
    signal?.removeEventListener("abort", abortFromParent);
  }
}

export function fetchJson(path, signal, timeoutMs = REQUEST_TIMEOUT_MS) {
  return fetchBody(path, signal, response => response.json(), timeoutMs);
}

export async function fetchBytes(path, signal, timeoutMs = REQUEST_TIMEOUT_MS) {
  return new Uint8Array(await fetchBody(path, signal,
    response => response.arrayBuffer(), timeoutMs));
}

function waitForRetry(delayMs, signal) {
  if (signal?.aborted) {
    const error = new Error("请求已取消");
    error.name = "AbortError";
    error.cancelled = true;
    return Promise.reject(error);
  }
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      signal?.removeEventListener("abort", abort);
      resolve();
    }, delayMs);
    const abort = () => {
      clearTimeout(timer);
      signal?.removeEventListener("abort", abort);
      const error = new Error("请求已取消");
      error.name = "AbortError";
      error.cancelled = true;
      reject(error);
    };
    signal?.addEventListener("abort", abort, { once: true });
  });
}

export function crc32(bytes) {
  let value = 0xffffffff;
  for (const byte of bytes) {
    value ^= byte;
    for (let bit = 0; bit < 8; bit++) {
      value = (value >>> 1) ^ ((value & 1) ? 0xedb88320 : 0);
    }
  }
  return ((value ^ 0xffffffff) >>> 0).toString(16).toUpperCase().padStart(8, "0");
}

function expectedCrc(value) {
  if (typeof value !== "string" || !/^0x[0-9a-f]{8}$/i.test(value)) {
    throw new Error("缺少有效的 CRC32 字段");
  }
  return value.slice(2).toUpperCase();
}

export async function getManifest(signal) {
  const manifest = await fetchJson("/api/evidence", signal);
  if (manifest.schema_version !== 2 ||
      !/^[0-9a-f]{8}-[0-9a-f]{8}-[0-9a-f]{8}$/i.test(manifest.token || "") ||
      manifest.page_size !== 4096 ||
      !Number.isInteger(manifest.total_bytes) ||
      manifest.total_bytes < 1 || manifest.total_bytes > MAX_SUMMARY_BYTES ||
      !Number.isInteger(manifest.raw_event_count) ||
      manifest.raw_event_count < 0 || manifest.raw_event_count > MAX_EVENTS) {
    throw new Error("板端 manifest 格式或容量不符合证据协议 v2");
  }
  expectedCrc(manifest.json_crc32);
  expectedCrc(manifest.content_crc32);
  return manifest;
}

async function readPages(url, size, step, signal) {
  const bytes = new Uint8Array(size);
  let offset = 0;
  while (offset < size) {
    const page = await fetchBytes(url(offset), signal);
    if (page.length === 0 || page.length > step || page.length > size - offset) {
      throw new Error(`分页长度异常，偏移 ${offset}`);
    }
    bytes.set(page, offset);
    offset += page.length;
  }
  return bytes;
}

export async function readSnapshot(manifest, signal) {
  const bytes = await readPages(
    offset => `/api/evidence/page/${manifest.token}/${offset}`,
    manifest.total_bytes, manifest.page_size, signal);
  if (crc32(bytes) !== expectedCrc(manifest.json_crc32)) {
    throw new Error("摘要 JSON CRC32 与 manifest 不一致");
  }
  const snapshot = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes));
  if (snapshot.schema_version !== 2 ||
      snapshot.snapshot_id !== manifest.snapshot_id ||
      snapshot.revision !== manifest.revision ||
      expectedCrc(snapshot.content_crc32) !== expectedCrc(manifest.content_crc32) ||
      !Array.isArray(snapshot.raw_events) ||
      snapshot.raw_events.length !== manifest.raw_event_count) {
    throw new Error("manifest 与证据摘要属于不同代或字段不一致");
  }
  return snapshot;
}

export async function readCurrent(initialManifest, signal) {
  let manifest = initialManifest;
  for (let attempt = 0; attempt < 3; attempt++) {
    try {
      return { manifest, snapshot: await readSnapshot(manifest, signal) };
    } catch (error) {
      if (signal?.aborted) throw error;
      if (error.status === 409 && attempt < 2) {
        manifest = await getManifest(signal);
        continue;
      }
      throw error;
    }
  }
  throw new Error("快照持续变化，请稍后重试");
}

export async function readEvent(manifest, event, signal) {
  const length = Number(event.captured_length);
  if (!Number.isInteger(length) || length < 0 || length > MAX_EVENT_BYTES) {
    throw new Error(`事件 #${event.sequence} 的捕获长度无效`);
  }
  const bytes = length === 0 ? new Uint8Array() : await readPages(
    offset => `/api/evidence/event/${manifest.token}/${event.sequence}/${offset}`,
    length, 4096, signal);
  if (crc32(bytes) !== expectedCrc(event.payload_crc32)) {
    throw new Error(`事件 #${event.sequence} 原始字节 CRC32 不一致`);
  }
  return bytes;
}

export async function exportEvidence(initialManifest, initialSnapshot, signal) {
  let manifest = initialManifest;
  let snapshot = initialSnapshot;
  for (let attempt = 0; attempt < 3; attempt++) {
    try {
      const payloads = {};
      for (const event of snapshot.raw_events) {
        if (!event.captured_length) continue;
        const bytes = await readEvent(manifest, event, signal);
        payloads[String(event.sequence)] = Array.from(bytes, byte =>
          byte.toString(16).padStart(2, "0")).join("");
      }
      return { ...snapshot, raw_payloads: payloads };
    } catch (error) {
      if (signal?.aborted) throw error;
      if (error.status === 409 && attempt < 2) {
        ({ manifest, snapshot } = await readCurrent(await getManifest(signal), signal));
        continue;
      }
      if ((error.status === 503 || error.status === 429) && attempt < 2) {
        await waitForRetry(error.retryAfterMs ?? 200 * (attempt + 1), signal);
        continue;
      }
      throw error;
    }
  }
  throw new Error("导出期间快照持续变化，请稍后重试");
}

export async function downloadEvidence(manifest, snapshot) {
  const complete = await exportEvidence(manifest, snapshot, AbortSignal.timeout(20000));
  const url = URL.createObjectURL(new Blob([JSON.stringify(complete, null, 2)],
    { type: "application/json" }));
  const link = document.createElement("a");
  link.href = url;
  link.download = `usb-evidence-${complete.snapshot_id}.json`;
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
  return complete;
}

export async function getHistory(signal) {
  const history = await fetchJson("/api/history", signal);
  if (history.history_type !== "summary_only" || !Array.isArray(history.items)) {
    throw new Error("历史摘要格式不符合预期");
  }
  return history.items;
}
