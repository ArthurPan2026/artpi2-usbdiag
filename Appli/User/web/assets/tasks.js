function node(tag, className, value) {
  const result = document.createElement(tag);
  if (className) result.className = className;
  if (value != null) result.textContent = String(value);
  return result;
}

let currentToken = null;
let answers = new Map();

function formControl(labelText, control) {
  const label = node("label", "task-control");
  label.append(node("span", "", labelText), control);
  return label;
}

function answerSelect(options, savedValue) {
  const select = node("select");
  select.append(new Option("请选择…", ""));
  for (const [label, value] of options) select.append(new Option(label, value));
  select.value = savedValue || "";
  return select;
}

function actualInterfaceVerdict(snapshot) {
  const result = snapshot?.rule_results?.find(rule => rule.id === "CFG_INTERFACE_COUNT");
  return result?.verdict || "NOT_OBSERVED";
}

function observedOrder(snapshot) {
  const get = kind => snapshot?.timeline?.find(item => item.kind === kind)?.event_sequence || 0;
  const prefix = get("DEVICE_8B");
  const address = get("SET_ADDRESS");
  if (!prefix || !address) return null;
  return prefix < address;
}

export function renderTeachingTasks(container, snapshot, token, onVerifyEvent) {
  if (currentToken !== token) {
    currentToken = token;
    answers = new Map();
  }
  const fragment = document.createDocumentFragment();
  const ruleResults = snapshot?.rule_results || [];
  const deviceRule = ruleResults.find(rule => rule.id === "DEV_FIELDS");
  const mpsMatch = deviceRule?.observed?.match(/bMaxPacketSize0\s*=\s*(\d+)/);
  const mpsExpected = mpsMatch ? mpsMatch[1] : null;
  const identityAvailable = Boolean(snapshot?.device?.present && mpsExpected &&
    deviceRule?.verdict !== "NOT_OBSERVED");

  const identityTask = node("article", "task-card");
  identityTask.append(node("p", "eyebrow", "任务 1 · 设备身份"),
    node("h3", "", "读出 VID:PID，并指出 EP0 包大小字段"),
    node("p", "small-note", identityAvailable ?
      "从当前冻结快照核对设备标识；EP0 包大小依据规则 DEV_FIELDS。" :
      "当前会话尚无足够的设备描述符证据，提交后会明确显示无法作答。"));
  const identityInput = node("input");
  identityInput.type = "text";
  identityInput.autocomplete = "off";
  identityInput.placeholder = "例如 1234:ABCD";
  identityInput.value = answers.get("identity")?.value || "";
  const mpsSelect = answerSelect([
    ["bMaxPacketSize0（设备描述符偏移 7）", "7"],
    ["wMaxPacketSize（端点描述符）", "4"],
    ["bInterval（端点描述符）", "6"],
  ], answers.get("identity")?.offset);
  identityTask.append(formControl("VID:PID", identityInput),
    formControl("EP0 包大小字段", mpsSelect));
  const identityStatus = node("p", "task-status", answers.get("identity")?.status || "尚未核验");
  const identityCheck = node("button", "button button-outline", "核对答案");
  identityCheck.type = "button";
  identityCheck.addEventListener("click", () => {
    const value = identityInput.value.trim().replace(/^0x/ig, "").replace(/\s/g, "").toUpperCase();
    const expected = snapshot?.device?.present ?
      `${Number(snapshot.device.vid).toString(16).padStart(4, "0")}:${Number(snapshot.device.pid).toString(16).padStart(4, "0")}`.toUpperCase() : null;
    let status;
    if (!identityAvailable || !expected) status = "无法作答：设备身份或 EP0 字段证据尚未观察到。";
    else if (value === expected && mpsSelect.value === "7") status = "正确：VID:PID 与 bMaxPacketSize0 偏移均与本次证据一致。";
    else status = `未通过：请对照设备描述符原始字节及 DEV_FIELDS；当前身份格式应为 XXXX:XXXX。`;
    answers.set("identity", { value: identityInput.value, offset: mpsSelect.value, status });
    identityStatus.textContent = status;
    identityStatus.classList.toggle("is-pass", status.startsWith("正确"));
  });
  identityTask.append(identityCheck, identityStatus);
  fragment.append(identityTask);

  const orderTask = node("article", "task-card");
  orderTask.append(node("p", "eyebrow", "任务 2 · 枚举顺序"),
    node("h3", "", "本次是否先读取 8 字节，再设置地址？"),
    node("p", "small-note", "只按带事件序号的已观察阶段回答，不把时间差解释成总线时序。"));
  const orderAnswer = answerSelect([["是", "yes"], ["否", "no"]], answers.get("order")?.value);
  orderTask.append(formControl("你的判断", orderAnswer));
  const orderStatus = node("p", "task-status", answers.get("order")?.status || "尚未核验");
  const orderCheck = node("button", "button button-outline", "核对顺序");
  orderCheck.type = "button";
  orderCheck.addEventListener("click", () => {
    const actual = observedOrder(snapshot);
    const value = orderAnswer.value;
    const status = actual == null ? "无法作答：这两个阶段中至少一个没有对应事件。" :
      value && (value === "yes") === actual ? "正确：事件序号支持该判断。" :
      "未通过：请比较 DEVICE_8B 与 SET_ADDRESS 的 Host 事件序号。";
    answers.set("order", { value, status });
    orderStatus.textContent = status;
    orderStatus.classList.toggle("is-pass", status.startsWith("正确"));
  });
  orderTask.append(orderCheck, orderStatus);
  fragment.append(orderTask);

  const interfaceTask = node("article", "task-card");
  interfaceTask.append(node("p", "eyebrow", "任务 3 · 接口与端点"),
    node("h3", "", "配置声明的接口数与描述符树是否一致？"),
    node("p", "small-note", "按规则 CFG_INTERFACE_COUNT 的本次实际判定回答；未捕获完整配置时不可猜测。"));
  const interfaceAnswer = answerSelect([
    ["一致（PASS）", "PASS"], ["不一致（FAIL）", "FAIL"],
    ["证据不足 / 未观察", "NOT_OBSERVED"], ["不适用", "NOT_APPLICABLE"],
  ], answers.get("interfaces")?.value);
  interfaceTask.append(formControl("你的判断", interfaceAnswer));
  const interfaceStatus = node("p", "task-status", answers.get("interfaces")?.status || "尚未核验");
  const interfaceCheck = node("button", "button button-outline", "核对接口关系");
  interfaceCheck.type = "button";
  interfaceCheck.addEventListener("click", () => {
    const actual = actualInterfaceVerdict(snapshot);
    const eligible = actual !== "NOT_OBSERVED" && snapshot?.capture_integrity === "COMPLETE";
    const status = !eligible ? "无法作答：当前冻结快照没有完整配置树证据。" :
      interfaceAnswer.value === actual ? "正确：回答与规则结果一致。" :
      "未通过：请查看期望/实测接口数及配置描述符来源。";
    answers.set("interfaces", { value: interfaceAnswer.value, status });
    interfaceStatus.textContent = status;
    interfaceStatus.classList.toggle("is-pass", status.startsWith("正确"));
  });
  interfaceTask.append(interfaceCheck, interfaceStatus);
  fragment.append(interfaceTask);

  const ruleTask = node("article", "task-card task-card-wide");
  ruleTask.append(node("p", "eyebrow", "任务 4 · 规则与原始字节"),
    node("h3", "", "选一条规则，核对判定并定位来源字节"),
    node("p", "small-note", "只有引用代号有效、偏移落在已捕获范围内且事件 CRC 校验成功，才会记为完成。"));
  const candidateRules = ruleResults.filter(rule => ["PASS", "FAIL"].includes(rule.verdict));
  const ruleSelect = node("select");
  ruleSelect.append(new Option(candidateRules.length ? "选择一条已判定规则…" : "当前无已判定规则", ""));
  for (const rule of candidateRules) ruleSelect.append(new Option(`${rule.id} · ${rule.verdict}`, rule.id));
  ruleSelect.value = answers.get("rule")?.ruleId || "";
  const verdictSelect = answerSelect([["PASS", "PASS"], ["FAIL", "FAIL"]], answers.get("rule")?.verdict);
  ruleTask.append(formControl("规则", ruleSelect), formControl("本次判定", verdictSelect));
  const ruleStatus = node("p", "task-status", answers.get("rule")?.status || "尚未核验");
  const ruleCheck = node("button", "button button-outline", "验证并定位字节");
  ruleCheck.type = "button";
  ruleCheck.addEventListener("click", async () => {
    const rule = candidateRules.find(item => item.id === ruleSelect.value);
    const reference = rule?.evidence_refs?.find(ref => ref.byte_length > 0);
    const event = snapshot?.raw_events?.find(item => item.sequence === reference?.event_sequence);
    if (!rule || !reference || !event || reference.byte_offset + reference.byte_length > event.captured_length) {
      ruleStatus.textContent = "无法作答：规则没有指向本代快照内有效的原始字节范围。";
      ruleStatus.classList.remove("is-pass");
      return;
    }
    if (verdictSelect.value !== rule.verdict) {
      ruleStatus.textContent = "未通过：规则判定与本次证据不一致。";
      ruleStatus.classList.remove("is-pass");
      return;
    }
    try {
      await onVerifyEvent(event);
      const status = `正确：已校验事件 #${event.sequence} 的 CRC，并定位偏移 ${reference.byte_offset}。` +
        (rule.omitted_failure_reference_count ? ` 另有 ${rule.omitted_failure_reference_count} 个失败位置未展开。` : "");
      answers.set("rule", { ruleId: rule.id, verdict: verdictSelect.value, status });
      ruleStatus.textContent = status;
      ruleStatus.classList.add("is-pass");
      container.dispatchEvent(new CustomEvent("usbdiag-task-locate", {
        detail: { rule, reference },
      }));
    } catch (error) {
      ruleStatus.textContent = `无法作答：原始字节未能完成校验（${error.message}）。`;
      ruleStatus.classList.remove("is-pass");
    }
  });
  ruleTask.append(ruleCheck, ruleStatus);
  if (ruleResults.some(rule => (rule.omitted_failure_reference_count || 0) > 0)) {
    ruleTask.append(node("p", "small-note", "提示：至少一条失败规则有更多失败位置未在四条引用上限中展开。"));
  }
  fragment.append(ruleTask);
  container.replaceChildren(fragment);
}
