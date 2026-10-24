import { getManifest, readCurrent, SNAPSHOT_READ_TIMEOUT_MS } from "./data.js";

const NORMAL_POLL_MS = 2500;
const RETRY_DELAYS_MS = [1000, 2000, 4000, 8000, 15000];

export function emptyEvidenceOutcome(phase) {
  if (phase === "busy") {
    return ["板端繁忙 · 自动重试中", "板端 Web 可达，证据接口暂时繁忙；页面会自动重试。", "warn"];
  }
  if (phase === "updating") {
    return ["证据仍在更新 · 自动重试中", "USB Host 快照在分页读取期间发生变化，已丢弃本轮数据并重新读取。", "warn"];
  }
  if (phase === "timeout") {
    return ["证据读取超时 · 自动重试中", "读取超过时限后已取消本轮请求；页面会自动重试。", "warn"];
  }
  if (phase === "offline") {
    return ["暂时无法连接开发板", "网络请求未完成；页面会自动重试，恢复后继续读取证据。", "bad"];
  }
  if (phase === "error") {
    return ["证据读取失败", "当前响应无法用于核验；可点击刷新重新读取。", "bad"];
  }
  return ["正在读取板端证据", "正在获取并校验当前 USB Host 快照。", "warn"];
}

export function createEvidencePoller({
  state, onStatus, renderAll, eventBySequence, rules, currentRule, chooseDefault,
}) {
  let active = null;
  let timer = null;
  let paused = false;
  let retryCount = 0;

  function clearTimer() {
    clearTimeout(timer);
    timer = null;
  }

  function schedule(delayMs) {
    clearTimer();
    if (paused || document.hidden || delayMs == null) return;
    timer = setTimeout(() => {
      timer = null;
      void refresh();
    }, delayMs);
  }

  function cancel(reason) {
    if (!active) return;
    active.cancelReason = reason;
    active.controller.abort();
  }

  function retryDelay(error) {
    const index = Math.min(retryCount, RETRY_DELAYS_MS.length - 1);
    retryCount++;
    return Math.max(RETRY_DELAYS_MS[index], Number(error.retryAfterMs) || 0);
  }

  function commitSnapshot(current) {
    const previousSession = state.snapshot?.session_id;
    const previousSequence = state.selectedEvent;
    const previousRule = state.selectedRule;
    state.byteTicket++;
    state.loadingKey = null;
    state.byteCache.clear();
    state.manifest = current.manifest;
    state.snapshot = current.snapshot;
    if (previousSession !== current.snapshot.session_id) {
      state.expandedNodes.clear();
      state.selectedNode = null;
      state.selectedDescriptor = null;
      state.showSerial = false;
    }
    if (previousSession === current.snapshot.session_id && eventBySequence(previousSequence)) {
      state.selectedEvent = previousSequence;
      state.selectedRule = rules().some(rule => rule.id === previousRule) ? previousRule : null;
      state.selectedReference = currentRule()?.evidence_refs?.[0] || null;
    } else chooseDefault(current.snapshot);
    renderAll();
  }

  async function refresh(force = false) {
    clearTimer();
    if (paused || document.hidden) return;
    if (active) {
      if (!force) return active.promise;
      cancel("manual");
      await active.promise;
      if (paused || document.hidden) return;
      if (active) return active.promise;
    }

    const operation = {
      controller: new AbortController(),
      cancelReason: null,
      promise: null,
    };
    active = operation;
    let deadlineExpired = false;
    let nextPollDelay = null;
    const deadline = setTimeout(() => {
      deadlineExpired = true;
      operation.controller.abort();
    }, SNAPSHOT_READ_TIMEOUT_MS);

    const current = state;
    if (!current.snapshot) {
      onStatus({ phase: "loading", connected: current.connected });
    }

    operation.promise = (async () => {
      try {
        const manifest = await getManifest(operation.controller.signal);
        /* A published token identifies an immutable, CRC-checked snapshot.
         * Force means probe now; it does not require downloading identical
         * pages again after a manual refresh or tab resume. */
        if (current.snapshot && manifest.token === current.manifest?.token) {
          retryCount = 0;
          onStatus({ phase: "ready", connected: true });
          nextPollDelay = NORMAL_POLL_MS;
          return;
        }

        const result = await readCurrent(manifest, operation.controller.signal);
        if (active !== operation || operation.cancelReason || paused || document.hidden) return;
        retryCount = 0;
        commitSnapshot(result);
        onStatus({ phase: "ready", connected: true, lastSync: new Date() });
        nextPollDelay = NORMAL_POLL_MS;
      } catch (error) {
        if (active !== operation || operation.cancelReason || paused || document.hidden) return;
        const reachable = Number.isInteger(error.status);
        const retryable = error.status == null || error.status === 408 ||
          error.status === 409 || error.status === 429 || error.status === 503 ||
          error.status >= 500;
        const delay = retryable ? retryDelay(error) : null;
        const retryMessage = delay == null ? "请点击刷新重新读取。" :
          `约 ${Math.max(1, Math.ceil(delay / 1000))} 秒后自动重试。`;
        let phase;
        let message;
        if (error.status === 503 || error.status === 429) {
          phase = "busy";
          message = `板端 Web 可达，但证据接口暂时繁忙；${retryMessage}`;
        } else if (error.status === 409) {
          phase = "updating";
          message = `证据在读取期间持续更新；已丢弃本轮分页数据，${retryMessage}`;
        } else if (deadlineExpired || error.status === 408) {
          phase = "timeout";
          message = `证据读取已超时并取消；${retryMessage}`;
        } else if (retryable) {
          phase = "offline";
          message = `证据读取失败：${error.message}；${retryMessage}`;
        } else {
          phase = "error";
          message = `证据读取失败：${error.message}；${retryMessage}`;
        }
        onStatus({ phase, connected: error.status === 503 || error.status === 429 ||
          (reachable && error.status !== 408), message });
        nextPollDelay = delay;
      } finally {
        clearTimeout(deadline);
        if (active === operation) {
          active = null;
          schedule(nextPollDelay);
        }
      }
    })();
    return operation.promise;
  }

  function pause() {
    paused = true;
    clearTimer();
    cancel("hidden");
  }

  function resume() {
    paused = false;
    return refresh(true);
  }

  return { start: () => refresh(true), refresh, pause, resume };
}
