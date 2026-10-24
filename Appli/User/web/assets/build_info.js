export async function readBuildInfo() {
  let response;
  let networkRecoveryUsed = false;
  while (true) {
    try {
      response = await fetch("/api/build", {
        cache: "no-store",
        signal: AbortSignal.timeout(3000),
      });
      break;
    } catch (error) {
      if (error.name !== "TypeError" || networkRecoveryUsed) {
        throw new Error(`构建身份接口连接失败（${error.name || "错误"}）：${error.message || "请求未完成"}`, { cause: error });
      }
      networkRecoveryUsed = true;
      await new Promise(resolve => setTimeout(resolve, 250));
    }
  }
  if (!response.ok) throw new Error(`构建身份 HTTP ${response.status}`);
  const result = await response.json();
  if (!/^[0-9a-f]{64}$/i.test(result.source_fingerprint || "")) {
    throw new Error("源码指纹缺失或格式错误");
  }
  return result;
}
