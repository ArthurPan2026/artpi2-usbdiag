let pendingUrls = new Set();
let verifiedReportBlobCache = null;

function pdfStageError(stage, message, cause) {
  const error = new Error(`${stage}：${message}`, { cause });
  error.pdfStage = stage;
  return error;
}

async function pdfResponse(path, stage, timeoutMs = 3000, retryBusy = true) {
  const maxAttempts = retryBusy ? 4 : 1;
  let busyAttempts = 0;
  let networkRecoveryUsed = false;
  while (busyAttempts < maxAttempts) {
    let response;
    try {
      response = await fetch(path, {
        cache: "no-store",
        signal: AbortSignal.timeout(timeoutMs),
      });
    } catch (error) {
      if (error.name === "TimeoutError") {
        throw pdfStageError(stage, `超时（${Math.ceil(timeoutMs / 1000)} 秒）`, error);
      }
      if (error.name === "TypeError" && !networkRecoveryUsed) {
        networkRecoveryUsed = true;
        await new Promise(resolve => setTimeout(resolve, 250));
        continue;
      }
      throw pdfStageError(stage,
        `连接失败（${error.name || "网络错误"}）：${error.message || "请求未完成"}`,
        error);
    }
    if (response.status === 503 && busyAttempts + 1 < maxAttempts) {
      busyAttempts++;
      await new Promise(resolve => setTimeout(resolve, 180 * busyAttempts));
      continue;
    }
    if (!response.ok) throw pdfStageError(stage, `HTTP ${response.status}`);
    return response;
  }
  throw pdfStageError(stage, "暂时繁忙，请稍后重试");
}

async function pdfJson(response, stage) {
  try {
    return await response.json();
  } catch (error) {
    throw pdfStageError(stage,
      `响应读取失败（${error.name || "网络错误"}）：${error.message || "响应不完整"}`,
      error);
  }
}

function clean(value, fallback = "未记录") {
  const text = String(value ?? "").replace(/[\u0000-\u001f\u007f]+/g, " ").trim();
  return text || fallback;
}

function keepUrlForViewer(viewer, url) {
  const timer = setInterval(() => {
    if (viewer.closed) {
      clearInterval(timer);
      URL.revokeObjectURL(url);
    }
  }, 1000);
}

export async function openVerificationPdf(snapshot, manifest, buildInfo, onProgress = () => {}) {
  const currentEvidenceToken = String(manifest?.token ?? "").toUpperCase();
  if (verifiedReportBlobCache?.token &&
      String(verifiedReportBlobCache.token).toUpperCase() !== currentEvidenceToken) {
    verifiedReportBlobCache = null;
  }
  const startedAt = performance.now();
  const phaseTimes = {};
  const elapsedSeconds = () => ((performance.now() - startedAt) / 1000).toFixed(1);
  const updateProgress = message => onProgress(String(message) + " · 已用 " + elapsedSeconds() + " 秒");
  try {
    if (!manifest?.token || !/^[0-9A-F-]{26}$/i.test(manifest.token)) {
      throw new Error("当前证据快照代号无效");
    }
    updateProgress("开发板正在冻结证据并生成中文 PDF…");
    let phaseStart = performance.now();
    const startResponse = await pdfResponse(
      "/api/report/start/" + encodeURIComponent(manifest.token), "启动板端 PDF 任务");
    const startInfo = await pdfJson(startResponse, "板端 PDF 任务信息");
    if (!Number.isInteger(startInfo.job_id) ||
        startInfo.evidence_token !== manifest.token) {
      throw new Error("开发板返回的 PDF 任务与当前快照不匹配");
    }
    const reportChunkSize = Number(startInfo.chunk_size);
    if (!Number.isInteger(reportChunkSize) || reportChunkSize < 1 ||
        reportChunkSize > 24576) {
      throw new Error("开发板返回的 PDF 分块大小无效");
    }
    phaseTimes.startMs = performance.now() - phaseStart;

    let reportInfo = startInfo;
    let polls = 0;
    if (reportInfo.state !== "ready") {
      updateProgress("PDF 正在板端排版…");
      while (polls++ < 240) {
        phaseStart = performance.now();
        const statusResponse = await pdfResponse(
          "/api/report/status/" + startInfo.job_id, "读取板端 PDF 任务状态");
        reportInfo = await pdfJson(statusResponse, "板端 PDF 状态");
        phaseTimes.statusMs = (phaseTimes.statusMs || 0) +
          performance.now() - phaseStart;
        if (reportInfo.job_id !== startInfo.job_id ||
            reportInfo.evidence_token !== manifest.token ||
            Number(reportInfo.chunk_size) !== reportChunkSize) {
          throw new Error("PDF 任务状态与证据代号不匹配");
        }
        if (reportInfo.state === "ready") break;
        if (reportInfo.state === "failed") {
          throw new Error("板端生成失败（代码 " + reportInfo.error_code + "）");
        }
        if (reportInfo.state !== "queued" && reportInfo.state !== "rendering") {
          throw new Error("板端 PDF 状态异常：" + reportInfo.state);
        }
        updateProgress("开发板正在生成报告…");
        await new Promise(resolve => setTimeout(resolve, 200));
      }
    }
    if (reportInfo?.state !== "ready") {
      throw new Error("板端 PDF 生成超时");
    }
    if (reportInfo.job_id !== startInfo.job_id ||
        reportInfo.evidence_token !== manifest.token ||
        Number(reportInfo.chunk_size) !== reportChunkSize) {
      throw new Error("PDF 任务状态与证据代号不匹配");
    }
    const totalBytes = Number(reportInfo.pdf_bytes);
    const crcText = String(reportInfo.pdf_crc32 || "");
    if (!/^0x[0-9a-f]{8}$/i.test(crcText)) {
      throw new Error("板端 PDF CRC 格式无效");
    }
    const expectedCrc = Number.parseInt(crcText.slice(2), 16) >>> 0;
    if (!Number.isInteger(totalBytes) || totalBytes < 5 ||
        totalBytes > 1024 * 1024 || !Number.isFinite(expectedCrc)) {
      throw new Error("板端 PDF 长度或 CRC 无效");
    }

    /* Reuse only the exact frozen report job returned by the board. Its
     * topology_generation is the job's generation, not a live USB query. */
    const cacheKey = JSON.stringify([
      String(buildInfo?.source_fingerprint || ""),
      String(buildInfo?.profile || ""),
      String(buildInfo?.build_type || ""),
      String(manifest.token).toUpperCase(),
      String(snapshot?.snapshot_id ?? ""),
      String(snapshot?.revision ?? ""),
      String(manifest.json_crc32 || "").toUpperCase(),
      String(manifest.content_crc32 || "").toUpperCase(),
      String(reportInfo.job_id),
      String(reportInfo.evidence_token).toUpperCase(),
      String(reportInfo.topology_generation ?? ""),
      totalBytes,
      expectedCrc,
    ]);
    let blob = verifiedReportBlobCache?.key === cacheKey ?
      verifiedReportBlobCache.blob : null;
    const cacheHit = blob instanceof Blob &&
      blob.type === "application/pdf" && blob.size === totalBytes;

    if (cacheHit) {
      phaseTimes.downloadMs = 0;
      phaseTimes.cacheHit = true;
      updateProgress("复用本页已校验的同一份 PDF…");
    } else {
      updateProgress("下载板端 PDF（" + totalBytes + " B）…");
      phaseStart = performance.now();
      let response = await pdfResponse(
        "/api/report/stream/" + startInfo.job_id,
        "接收板端 PDF 数据流", 35000, false);
      const contentType = String(response.headers.get("Content-Type") || "")
        .split(";", 1)[0].trim().toLowerCase();
      const contentLength = Number(response.headers.get("Content-Length"));
      if (contentType !== "application/pdf" || contentLength !== totalBytes) {
        throw new Error("板端 PDF 流响应类型或长度与任务信息不匹配");
      }
      let pdfBody;
      let bodyNetworkRecoveryUsed = false;
      while (true) {
        try {
          pdfBody = await response.arrayBuffer();
          break;
        } catch (error) {
          const timeout = error.name === "TimeoutError" || error.name === "AbortError";
          if (!timeout && error.name === "TypeError" && !bodyNetworkRecoveryUsed) {
            bodyNetworkRecoveryUsed = true;
            updateProgress("网络连接短暂中断，正在重新接收同一份板端报告…");
            await response.body?.cancel().catch(() => {});
            await new Promise(resolve => setTimeout(resolve, 250));
            const retryResponse = await pdfResponse(
              "/api/report/stream/" + startInfo.job_id,
              "恢复板端 PDF 数据流", 35000, false);
            const retryType = String(retryResponse.headers.get("Content-Type") || "")
              .split(";", 1)[0].trim().toLowerCase();
            const retryLength = Number(retryResponse.headers.get("Content-Length"));
            if (retryType !== "application/pdf" || retryLength !== totalBytes) {
              throw pdfStageError("恢复板端 PDF 数据流", "响应类型或长度不匹配");
            }
            response = retryResponse;
            continue;
          }
          throw pdfStageError("PDF 正文接收",
            `${timeout ? "超时或被取消" : "中断"}（${error.name || "网络错误"}）：${error.message || "响应体未完整到达"}`,
            error);
        }
      }
      if (pdfBody.byteLength !== totalBytes) {
        throw new Error("板端 PDF 流长度异常：期望 " + totalBytes +
          " B，收到 " + pdfBody.byteLength + " B");
      }
      const pdfBytes = new Uint8Array(pdfBody);
      phaseTimes.downloadMs = performance.now() - phaseStart;
      updateProgress("PDF 数据已接收，正在校验内容…");
      if (String.fromCharCode(...pdfBytes.subarray(0, 5)) !== "%PDF-") {
        throw new Error("板端返回内容不是 PDF");
      }
      phaseStart = performance.now();
      let crc = 0xFFFFFFFF;
      for (const byte of pdfBytes) {
        crc ^= byte;
        for (let bit = 0; bit < 8; bit++) {
          crc = (crc >>> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
        }
      }
      crc = (crc ^ 0xFFFFFFFF) >>> 0;
      phaseTimes.crcMs = performance.now() - phaseStart;
      if (crc !== expectedCrc) {
        throw new Error("PDF CRC 校验失败：期望 0x" +
          expectedCrc.toString(16) + "，收到 0x" + crc.toString(16));
      }
      phaseStart = performance.now();
      blob = new Blob([pdfBytes], { type: "application/pdf" });
      phaseTimes.blobMs = performance.now() - phaseStart;
      if (!(blob instanceof Blob) || blob.size !== totalBytes) {
        throw new Error("PDF 输出长度不一致");
      }
      verifiedReportBlobCache = { key: cacheKey, token: manifest.token, blob };
      phaseTimes.cacheHit = false;
    }
    if (!(blob instanceof Blob) || blob.size !== totalBytes) {
      throw new Error("PDF 输出长度不一致");
    }
    const revision = clean(snapshot?.revision, "0");
    const fileName = "usb-verification-" +
      clean(snapshot?.snapshot_id, "0") + "-r" + revision + ".pdf";
    const url = URL.createObjectURL(blob);
    const elapsedMs = Math.round(performance.now() - startedAt);
    updateProgress(cacheHit ?
      "已复用并核对本页缓存的 PDF，正在打开新标签页…" :
      "PDF 已完整接收并通过 CRC 校验，正在打开新标签页…");
    let viewer = null;
    try {
      viewer = window.open(url, "_blank");
    } catch {
      /* Keep the verified PDF available through the in-page preview link. */
    }
    if (viewer && !viewer.closed) {
      try { viewer.opener = null; } catch { /* Preview may be cross-origin. */ }
      keepUrlForViewer(viewer, url);
      updateProgress("板端 PDF 已校验并打开预览，可从浏览器工具栏下载或打印");
      return { previewOpened: true, fileName, elapsedMs, phaseTimes };
    }
    pendingUrls.add(url);
    updateProgress(cacheHit ?
      "已复用并校验缓存 PDF；浏览器拦截了新标签页，请点击下方链接打开预览" :
      "PDF 已完整接收并校验；浏览器拦截了新标签页，请点击下方链接打开预览");
    return { previewOpened: false, fallbackUrl: url, fileName, elapsedMs, phaseTimes };
  } catch (error) {
    throw error;
  }
}
if (typeof window !== "undefined") {
  window.addEventListener("pagehide", () => {
    for (const url of pendingUrls) URL.revokeObjectURL(url);
    pendingUrls.clear();
  }, { once: true });
}
