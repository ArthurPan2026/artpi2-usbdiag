export function lazyAsset(path) {
  let module = null;
  let pending = null;
  let attempt = 0;
  let failures = 0;
  let retryAt = 0;
  let waitTimer = null;
  let waitResolve = null;
  const backoff = [250, 500, 1000, 2000, 4000];
  const load = () => {
    if (module) return Promise.resolve(module);
    if (pending) return pending;
    if (failures >= backoff.length) {
      return Promise.reject(new Error(`${path}自动重试已暂停，请手动重试。`));
    }
    if (Date.now() < retryAt) {
      let waitPromise;
      waitPromise = new Promise(resolve => {
        waitResolve = resolve;
        waitTimer = setTimeout(() => {
          waitTimer = null;
          waitResolve = null;
          if (pending === waitPromise) pending = null;
          resolve(load());
        }, retryAt - Date.now());
      });
      pending = waitPromise;
      return waitPromise;
    }
    const separator = path.includes("?") ? "&" : "?";
    pending = (async () => {
      let timeoutId;
      try {
        const timeout = new Promise((resolve, reject) => {
          timeoutId = setTimeout(() => reject(new Error(`${path}模块加载超过 3 秒。`)), 3000);
        });
        module = await Promise.race([
          import(`${path}${separator}asset_retry=${++attempt}`), timeout,
        ]);
        failures = 0;
        retryAt = 0;
        return module;
      } catch (error) {
        failures++;
        retryAt = Date.now() + backoff[Math.min(failures - 1, backoff.length - 1)];
        throw error;
      } finally {
        clearTimeout(timeoutId);
      }
    })().finally(() => { pending = null; });
    return pending;
  };
  load.reset = () => {
    failures = 0;
    retryAt = 0;
    if (waitTimer !== null) {
      clearTimeout(waitTimer);
      waitTimer = null;
      const resume = waitResolve;
      waitResolve = null;
      pending = null;
      resume?.(load());
    }
  };
  return load;
}
