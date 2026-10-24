import { crc32, fetchJson } from "./data.js";

const $ = id => document.getElementById(id);
const PAGE_SIZE = 1200;
let activeManifest = null;
let shownGeneration = null;
let lastManifestSignature = null;
let lastRenderedSignature = null;
let activeRefresh = null;
const activeConfigReads = new Set();
const TOPOLOGY_TIMEOUT_MS = 20000;
/* Mongoose's TCP retransmission timer starts at 1 s and backs off
 * exponentially. Allow one more retransmission window than the generic
 * evidence API budget so a transient Wi-Fi loss does not discard a healthy
 * topology request before TCP can recover. */
const TOPOLOGY_REQUEST_TIMEOUT_MS = 8000;
let lastTopologyResult = null;

export class UsbTopologyChangedError extends Error {
  constructor(message = "USB 清单在读取期间变化") {
    super(message);
    this.name = "UsbTopologyChangedError";
    this.status = 409;
  }
}

export function number(value, fallback = 0) {
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : fallback;
}

function setText(id, value) {
  const target = $(id);
  if (target) target.textContent = value == null ? "--" : String(value);
}

export function manifestSignature(manifest) {
  return JSON.stringify([
    manifest.generation, manifest.present, manifest.enumeration_complete,
    manifest.port, manifest.hub, manifest.speed, manifest.vid, manifest.pid,
    manifest.manufacturer, manifest.product, manifest.serial,
    manifest.bcd_usb, manifest.bcd_device, manifest.declared_configurations,
    manifest.stored_configurations, manifest.active_config_index,
    manifest.config_declared_bytes, manifest.config_captured_bytes,
    manifest.config_crc32, manifest.config_complete, manifest.config_truncated,
    manifest.interface_count, manifest.endpoint_count, manifest.string_count,
    manifest.class_bind_failures, manifest.unknown_interface_count,
    manifest.node_truncated, manifest.interface_truncated,
    manifest.endpoint_truncated, manifest.overflow_reason,
    manifest.device_descriptor || null,
    (manifest.configurations || []).map(config => [
      config.index, config.value, config.declared_length, config.captured_length,
      config.complete, config.parsed, config.malformed, config.truncated,
      config.status,
    ]),
  ]);
}

export function validManifest(manifest) {
  return manifest && manifest.schema_version === 1 &&
    Number.isInteger(Number(manifest.generation)) &&
    typeof manifest.present === "boolean" &&
    Number.isInteger(Number(manifest.interface_count)) &&
    Number.isInteger(Number(manifest.endpoint_count)) &&
    Number.isInteger(Number(manifest.string_count)) &&
    Array.isArray(manifest.configurations);
}

function decodeDeviceDescriptor(bytes, manifest) {
  if (bytes.length !== 18 || bytes[0] !== 18 || bytes[1] !== 1) {
    throw new Error("设备描述符长度或类型无效");
  }
  const vid = bytes[8] | (bytes[9] << 8);
  const pid = bytes[10] | (bytes[11] << 8);
  const bcdUsb = bytes[2] | (bytes[3] << 8);
  const bcdDevice = bytes[12] | (bytes[13] << 8);
  if (vid !== Number(manifest.vid) || pid !== Number(manifest.pid) ||
      bcdUsb !== Number(manifest.bcd_usb) || bcdDevice !== Number(manifest.bcd_device)) {
    throw new UsbTopologyChangedError("设备描述符与清单身份不一致");
  }
  return {
    bytes,
    manufacturerIndex: bytes[14],
    productIndex: bytes[15],
    serialIndex: bytes[16],
  };
}

function arraysMatch(value, count, field) {
  return value && Array.isArray(value.items) &&
    Number(value.count) === count && value.items.length === count &&
    value.items.every(item => item && typeof item === "object" && !Array.isArray(item)) &&
    (field !== "interfaces" || value.items.every(item =>
      Number.isInteger(Number(item.number)) && Number.isInteger(Number(item.endpoint_count))));
}

function topologyIsPartial(manifest) {
  return !manifest.config_complete || manifest.config_truncated ||
    manifest.interface_truncated || manifest.endpoint_truncated ||
    (manifest.configurations || []).some(config =>
      !config.complete || config.truncated || config.malformed || !config.parsed);
}

export async function topologyRequest(path, reader, signal) {
  const startedAt = performance.now();
  try {
    return await reader(path, signal, TOPOLOGY_REQUEST_TIMEOUT_MS);
  } catch (error) {
    if (error && typeof error === "object") {
      error.requestPath ||= path;
      error.elapsedMs ??= Math.round(performance.now() - startedAt);
    }
    if (error?.name === "TypeError") error.message = `${path}: ${error.message}`;
    throw error;
  }
}

export function topologyJson(path, signal) {
  return topologyRequest(path, fetchJson, signal);
}

async function readUsbTopologyBatch(signal) {
  const before = await topologyJson("/api/usb/manifest", signal);
  if (!validManifest(before)) throw new Error("USB manifest 格式无效");
  if (!before.present) return { state: "absent", manifest: before };
  if (!before.enumeration_complete) return { state: "incomplete", manifest: before };

  const interfaceList = await topologyJson("/api/usb/interfaces", signal);
  const endpointList = await topologyJson("/api/usb/endpoints", signal);
  const stringList = await topologyJson("/api/usb/strings", signal);
  if (!Array.isArray(before.device_descriptor) || before.device_descriptor.length !== 18 ||
      before.device_descriptor.some(byte => !Number.isInteger(byte) || byte < 0 || byte > 255)) {
    throw new Error("设备描述符字段无效");
  }
  const deviceDescriptor = decodeDeviceDescriptor(
    Uint8Array.from(before.device_descriptor), before);
  const after = await topologyJson("/api/usb/manifest", signal);
  if (!validManifest(after) || manifestSignature(before) !== manifestSignature(after) ||
      !after.present || !after.enumeration_complete) {
    throw new UsbTopologyChangedError();
  }
  if (!arraysMatch(interfaceList, Number(before.interface_count), "interfaces") ||
      !arraysMatch(endpointList, Number(before.endpoint_count), "endpoints") ||
      !arraysMatch(stringList, Number(before.string_count), "strings")) {
    throw new Error("USB 列表计数与 manifest 不一致，已丢弃本批数据");
  }
  const endpointCountsMatch = interfaceList.items.every(info => {
    const matches = endpointList.items.filter(endpoint =>
      Number(endpoint.config) === Number(info.config) &&
      Number(endpoint.interface) === Number(info.number) &&
      Number(endpoint.alternate) === Number(info.alternate));
    return matches.length === Number(info.endpoint_count);
  });
  if (!endpointCountsMatch) throw new Error("接口与端点清单不一致，已丢弃本批数据");

  return {
    state: topologyIsPartial(before) ? "partial" : "stable",
    manifest: before,
    interfaces: interfaceList.items,
    endpoints: endpointList.items,
    strings: stringList.items.filter(item => Number(item.index) !== deviceDescriptor.serialIndex),
    deviceDescriptor,
  };
}

export async function readUsbTopology(parentSignal) {
  const batchController = new AbortController();
  const abortBatch = () => batchController.abort();
  if (parentSignal?.aborted) batchController.abort();
  else parentSignal?.addEventListener("abort", abortBatch, { once: true });
  try {
    return await readUsbTopologyBatch(batchController.signal);
  } catch (error) {
    batchController.abort();
    throw error;
  } finally {
    parentSignal?.removeEventListener("abort", abortBatch);
  }
}

function endpointType(attributes) {
  return ["控制", "等时", "批量", "中断"][number(attributes) & 0x03] || "未知";
}

function endpointDirection(address) {
  return (number(address) & 0x80) !== 0 ? "IN" : "OUT";
}

function byteHex(value, width = 2) {
  return number(value).toString(16).toUpperCase().padStart(width, "0");
}

function element(tag, className, value) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (value != null) node.textContent = String(value);
  return node;
}

function formatHex(bytes) {
  const lines = [];
  for (let offset = 0; offset < bytes.length; offset += 16) {
    const group = Array.from(bytes.slice(offset, offset + 16), value => byteHex(value)).join(" ");
    lines.push(`${offset.toString(16).toUpperCase().padStart(4, "0")}  ${group}`);
  }
  return lines.join("\n");
}

function clearChildren(target) {
  target.replaceChildren();
}

function renderSummary(result) {
  const target = $("usb-topology-summary");
  clearChildren(target);
  const manifest = result.manifest;
  if (!manifest?.present) return;
  const configs = `${number(manifest.stored_configurations)}/${number(manifest.declared_configurations)}`;
  const integrity = manifest.config_complete && !manifest.config_truncated ? "完整" : "部分捕获";
  const bind = number(manifest.class_bind_failures) === 0 ? "未见失败" : `${manifest.class_bind_failures} 项失败`;
  const rows = [
    ["Host 观察速度", manifest.speed_name || "未知"],
    ["配置", configs],
    ["配置字节", `${number(manifest.config_captured_bytes)} / ${number(manifest.config_declared_bytes)}`],
    ["接口 / 端点", `${number(manifest.interface_count)} / ${number(manifest.endpoint_count)}`],
    ["解析完整性", integrity],
    ["驱动绑定", bind],
  ];
  for (const [label, value] of rows) {
    const card = element("div", "usb-summary-card");
    card.append(element("span", "", label));
    const strong = element("strong", "", value);
    if (label === "解析完整性") strong.classList.add(integrity === "完整" ? "is-good" : "is-warning");
    if (label === "驱动绑定" && number(manifest.class_bind_failures) > 0) strong.classList.add("is-error");
    card.append(strong);
    target.append(card);
  }
}

function renderInterfaces(result) {
  const target = $("usb-interface-list");
  const manifest = result.manifest;
  clearChildren(target);
  setText("usb-interface-count", `${result.interfaces.length} / ${number(manifest.interface_count)} 个接口`);
  if (!result.interfaces.length) {
    target.append(element("p", "small-note", "当前清单没有可展示的接口记录。"));
    return;
  }
  for (const info of result.interfaces) {
    const card = element("article", "usb-interface-card");
    const heading = element("div", "usb-interface-heading");
    heading.append(element("strong", "", `配置 ${number(info.config) + 1} · 接口 ${number(info.number)}${number(info.alternate) ? ` · Alternate ${number(info.alternate)}` : ""}`));
    card.append(heading);
    const className = info.class || `未知类别 0x${byteHex(info.class_code)}`;
    card.append(element("p", "small-note", className));
    const meta = element("div", "usb-class-meta");
    meta.append(element("span", "", `Class ${byteHex(info.class_code)}`));
    meta.append(element("span", "", `Sub ${byteHex(info.subclass)}`));
    meta.append(element("span", "", `Protocol ${byteHex(info.protocol)}`));
    card.append(meta);
    const status = info.driver_bound ?
      `Host 类驱动已绑定${info.driver ? ` · ${info.driver}` : ""}` :
      info.driver_failed ? `Host 类驱动绑定失败${info.driver ? ` · ${info.driver}` : ""}` :
      "Host 类驱动未报告绑定";
    const driver = element("p", `usb-driver-status${info.driver_bound ? " is-good" : info.driver_failed ? " is-error" : " is-warning"}`, status);
    card.append(driver);

    const endpoints = result.endpoints.filter(endpoint =>
      Number(endpoint.config) === Number(info.config) &&
      Number(endpoint.interface) === Number(info.number) &&
      Number(endpoint.alternate) === Number(info.alternate));
    if (endpoints.length) {
      const table = element("table", "usb-endpoints");
      const head = document.createElement("thead");
      const row = document.createElement("tr");
      for (const title of ["端点", "传输", "最大包长", "间隔"]) row.append(element("th", "", title));
      head.append(row);
      const body = document.createElement("tbody");
      for (const endpoint of endpoints) {
        const item = document.createElement("tr");
        item.append(element("td", "", `0x${byteHex(endpoint.address)} ${endpointDirection(endpoint.address)}`));
        item.append(element("td", "", endpointType(endpoint.attributes)));
        item.append(element("td", "", `${number(endpoint.max_packet_size)} 字节`));
        item.append(element("td", "", number(endpoint.interval)));
        body.append(item);
      }
      table.append(head, body);
      card.append(table);
    } else {
      card.append(element("p", "small-note", "该接口未观察到端点描述符。"));
    }
    target.append(card);
  }
}

function renderStrings(result) {
  const target = $("usb-string-list");
  clearChildren(target);
  setText("usb-string-count", `${result.strings.length} 项（序列号已遮盖）`);
  if (!result.strings.length) {
    target.append(element("p", "small-note", "当前没有可显示的非序列号字符串。"));
    return;
  }
  for (const item of result.strings) {
    const card = element("div", "usb-string-card");
    card.append(element("span", "", `字符串 #${number(item.index)} · 捕获 ${number(item.captured_length)} / ${number(item.declared_length)} 字节`));
    card.append(element("strong", "", item.text || "（空字符串）"));
    target.append(card);
  }
}

function renderDescriptors(result) {
  const target = $("usb-descriptor-list");
  clearChildren(target);
  const device = document.createElement("details");
  device.className = "usb-descriptor-block";
  device.append(element("summary", "", "设备描述符 · 18 / 18 字节"));
  device.append(element("pre", "", formatHex(result.deviceDescriptor.bytes)));
  target.append(device);
  for (const config of result.manifest.configurations || []) {
    const block = document.createElement("details");
    block.className = "usb-descriptor-block";
    const complete = config.complete && !config.truncated ? "完整" : "部分捕获";
    const summary = element("summary", "", `配置 ${number(config.index) + 1} · ${number(config.captured_length)} / ${number(config.declared_length)} 字节 · ${complete}`);
    const button = element("button", "button button-quiet", "读取配置原始字节");
    button.type = "button";
    const output = element("pre", "", "展开后读取；结果将按当前 USB 代次和 CRC 复核。");
    block.append(summary, button, output);
    button.addEventListener("click", async () => {
      button.disabled = true;
      button.textContent = "读取中…";
      const operation = { controller: new AbortController(), cancelReason: null };
      activeConfigReads.add(operation);
      let deadlineExpired = false;
      const timeout = setTimeout(() => {
        deadlineExpired = true;
        operation.controller.abort();
      }, TOPOLOGY_TIMEOUT_MS);
      try {
        const { readConfigRaw } = await import("./topology_raw.js");
        const bytes = await readConfigRaw(config, result.manifest,
          operation.controller.signal);
        output.textContent = formatHex(bytes);
        button.textContent = `CRC32 ${crc32(bytes)}`;
      } catch (error) {
        if (!operation.cancelReason) {
          output.textContent = deadlineExpired ?
            "未显示原始数据：读取超时（20 秒）" :
            `未显示原始数据：${error.message}`;
        }
        button.textContent = "重新读取配置字节";
      } finally {
        clearTimeout(timeout);
        activeConfigReads.delete(operation);
        button.disabled = false;
      }
    });
    target.append(block);
  }
}

function renderEmpty(message = "", manifest = null) {
  activeManifest = null;
  shownGeneration = null;
  lastTopologyResult = manifest ? { state: "absent", manifest } : null;
  if (manifest) {
    lastManifestSignature = manifestSignature(manifest);
    lastRenderedSignature = `absent:${lastManifestSignature}`;
  }
  setText("usb-device-title", "未检测到连接设备");
  setText("usb-device-meta", "等待 USB Host 观察到设备。");
  setText("usb-generation", "代次 --");
  setText("usb-interface-count", "无接口");
  setText("usb-string-count", "无字符串");
  $("usb-topology-summary").replaceChildren();
  $("usb-interface-list").replaceChildren(element("p", "small-note", "尚无设备接口记录。"));
  $("usb-string-list").replaceChildren(element("p", "small-note", "尚无设备字符串记录。"));
  $("usb-descriptor-list").replaceChildren();
  const status = $("usb-topology-status");
  status.hidden = !message;
  status.className = "usb-topology-status is-warning";
  status.textContent = message;
}

function showStatus(message, tone = "warning") {
  const status = $("usb-topology-status");
  status.hidden = !message;
  status.className = `usb-topology-status${tone === "error" ? " is-error" : tone === "warning" ? " is-warning" : ""}`;
  status.textContent = message;
}

function showResultStatus(result) {
  const manifest = result.manifest;
  if (result.state === "incomplete") {
    showStatus("USB Host 仍在收集描述符或绑定类别驱动，完整拓扑尚未就绪。", "warning");
  } else if (result.state === "partial") {
    showStatus(`拓扑来自 USB 代次 ${number(manifest.generation)}；描述符或列表存在截断/解析异常，当前接口只代表已捕获部分。`, "warning");
  } else if (number(manifest.class_bind_failures) > 0) {
    showStatus(`Host 已观察到 ${number(manifest.class_bind_failures)} 项类别驱动绑定失败；这不等于 HID 报告收发结果。`, "warning");
  } else {
    showStatus(`USB 代次 ${number(manifest.generation)} 的描述符清单在读取期间保持一致。`, "info");
  }
}

function renderResult(result) {
  const manifest = result.manifest;
  lastTopologyResult = result;
  lastManifestSignature = manifestSignature(manifest);
  activeManifest = result.state === "stable" ? manifest : null;
  shownGeneration = result.state === "stable" ? number(manifest.generation) : null;
  const product = manifest.product || "未提供产品名称";
  setText("usb-device-title", `${product} · ${byteHex(manifest.vid, 4)}:${byteHex(manifest.pid, 4)}`);
  setText("usb-device-meta", `${manifest.manufacturer || "未提供厂商名称"} · ${manifest.speed_name || "速度未知"} · Host 端口 ${number(manifest.port)}`);
  setText("usb-generation", `代次 ${number(manifest.generation)}`);
  renderSummary(result);
  if (result.state === "incomplete") {
    $("usb-interface-list").replaceChildren(element("p", "small-note", "枚举尚未完成；当前接口计数不能当作完整清单。"));
    $("usb-string-list").replaceChildren(element("p", "small-note", "枚举尚未完成；字符串清单暂不展示。"));
    $("usb-descriptor-list").replaceChildren();
    setText("usb-interface-count", `${number(manifest.interface_count)} 个暂存接口`);
    setText("usb-string-count", "枚举未完成");
    showResultStatus(result);
    return;
  }
  renderInterfaces(result);
  renderStrings(result);
  renderDescriptors(result);
  showResultStatus(result);
}

function resultSignature(result) {
  return JSON.stringify([
    result.state,
    manifestSignature(result.manifest),
    result.interfaces || [],
    result.endpoints || [],
    result.strings || [],
    Array.from(result.deviceDescriptor?.bytes || []),
  ]);
}

export function cancelTopologyRefresh(reason = "cancelled") {
  if (activeRefresh) {
    activeRefresh.cancelReason = reason;
    activeRefresh.controller.abort();
  }
  for (const operation of activeConfigReads) {
    operation.cancelReason = reason;
    operation.controller.abort();
  }
}

export async function refreshTopology(force = false) {
  if (document.hidden) return false;
  if (activeRefresh) {
    /* Join the current read; do not abort sockets to restart the same batch. */
    if (!activeRefresh.cancelReason) return activeRefresh.promise;
    await activeRefresh.promise;
    if (document.hidden) return false;
    if (activeRefresh) return activeRefresh.promise;
  }
  const operation = {
    controller: new AbortController(),
    cancelReason: null,
    promise: null,
  };
  activeRefresh = operation;
  const button = $("usb-refresh-button");
  if (button) button.disabled = true;
  let deadlineExpired = false;
  const timeout = setTimeout(() => {
    deadlineExpired = true;
    operation.controller.abort();
  }, TOPOLOGY_TIMEOUT_MS);
  operation.promise = (async () => {
    try {
      /* Reuse the validated batch when its immutable signature is unchanged. */
      if (lastManifestSignature !== null && (!force || activeManifest !== null)) {
        const probe = await topologyJson("/api/usb/manifest", operation.controller.signal);
        if (!validManifest(probe)) throw new Error("USB manifest 格式无效");
        if (manifestSignature(probe) === lastManifestSignature) {
          /* The previous warning is cleared only after confirming the same generation. */
          if (lastTopologyResult?.state === "absent") showStatus("", "info");
          else if (lastTopologyResult) showResultStatus(lastTopologyResult);
          return true;
        }
      }
      const result = await readUsbTopology(operation.controller.signal);
      if (activeRefresh !== operation || operation.cancelReason || document.hidden) return false;
      if (result.state === "absent") {
        const signature = `absent:${manifestSignature(result.manifest)}`;
        lastManifestSignature = manifestSignature(result.manifest);
        lastTopologyResult = result;
        if (signature !== lastRenderedSignature) renderEmpty("", result.manifest);
        else {
          activeManifest = null;
          shownGeneration = null;
          showStatus("", "info");
        }
      } else {
        const signature = resultSignature(result);
        if (signature !== lastRenderedSignature) {
          renderResult(result);
          lastRenderedSignature = signature;
        } else {
          lastManifestSignature = manifestSignature(result.manifest);
          activeManifest = result.state === "stable" ? result.manifest : null;
          shownGeneration = result.state === "stable" ? number(result.manifest.generation) : null;
          lastTopologyResult = result;
          showResultStatus(result);
        }
      }
      return true;
    } catch (error) {
      if (activeRefresh !== operation || operation.cancelReason || document.hidden) return false;
      const messageHasPath = error.requestPath &&
        String(error.message || "").includes(error.requestPath);
      const requestContextParts = [];
      if (error.requestPath && !messageHasPath) {
        requestContextParts.push(`接口 ${error.requestPath}`);
      }
      if (Number.isFinite(error.elapsedMs)) {
        requestContextParts.push(`耗时 ${error.elapsedMs} ms`);
      }
      const requestContext = requestContextParts.join("，");
      const message = deadlineExpired ? "拓扑读取超时（20 秒）" :
        error.timedOut ?
          `拓扑请求超时（${Math.ceil(error.timeoutMs / 1000)} 秒${requestContext ? `；${requestContext}` : ""}）` :
        error.status === 408 ?
          `拓扑接口返回 HTTP 408${requestContext ? `（${requestContext}）` : ""}` :
        error instanceof UsbTopologyChangedError ?
          "USB 状态发生变化；本批清单已丢弃，现有内容保持不变。" :
          `USB 拓扑读取失败${requestContext ? `（${requestContext}）` : ""}：${error.message}`;
      if (shownGeneration != null) {
        showStatus(`仍显示上次校验的 USB 代次 ${shownGeneration}；${message}`, "warning");
      } else if (lastTopologyResult) {
        showStatus(message, error instanceof UsbTopologyChangedError ? "warning" : "error");
      } else {
        renderEmpty(message);
        if (!(error instanceof UsbTopologyChangedError)) showStatus(message, "error");
      }
      return false;
    } finally {
      clearTimeout(timeout);
      if (activeRefresh === operation) {
        activeRefresh = null;
        if (button) button.disabled = false;
      }
    }
  })();
  return operation.promise;
}

export function currentTopologyManifest() { return activeManifest; }

