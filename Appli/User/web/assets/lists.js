function element(tag, className, label) {
  const value = document.createElement(tag);
  if (className) value.className = className;
  if (label != null) value.textContent = String(label);
  return value;
}

export function renderEvents(target, source, selectedSequence, search, filter,
                             requestName, formatUs, errorName, onSelect) {
  const visible = source.filter(event => {
    if (filter !== "all" && event.type !== filter) return false;
    return `${event.sequence} ${event.type} ${requestName(event)} ${event.return_status}`
      .toLowerCase().includes(search);
  });
  const fragment = document.createDocumentFragment();
  for (const event of visible) {
    const button = element("button", `list-item${selectedSequence === event.sequence ? " is-selected" : ""}`);
    button.type = "button";
    button.dataset.sequence = String(event.sequence);
    button.append(element("strong", "", `#${event.sequence} · ${requestName(event)}`),
      element("span", "", `${formatUs(event.timestamp_us)} · ${event.return_status < 0 ? errorName(event.return_status) : event.capture_status || event.type}`));
    button.addEventListener("click", () => onSelect(event));
    fragment.append(button);
  }
  if (!visible.length) fragment.append(element("p", "small-note", "没有匹配的事件。"));
  target.replaceChildren(fragment);
  return `${visible.length} / ${source.length} 条`;
}

export function renderRules(target, source, selectedRule, search, filter,
                            ruleInfo, verdictLabel, snapshot, onSelect) {
  const visible = source.filter(rule =>
    (filter === "all" || rule.verdict === filter) &&
    `${rule.id} ${ruleInfo(rule).title} ${rule.expected} ${rule.observed}`
      .toLowerCase().includes(search));
  const fragment = document.createDocumentFragment();
  for (const rule of visible) {
    const info = ruleInfo(rule);
    const button = element("button", `list-item${selectedRule === rule.id ? " is-selected" : ""}`);
    button.type = "button";
    button.dataset.rule = rule.id;
    button.append(element("strong", "", `${info.title} · ${verdictLabel(rule, snapshot)}`),
      element("span", "", `${rule.id} · ${rule.clause || "依据未记录"}`));
    button.addEventListener("click", () => onSelect(rule));
    fragment.append(button);
  }
  if (!visible.length) fragment.append(element("p", "small-note", "没有匹配的规则。"));
  target.replaceChildren(fragment);
  return `${visible.length} / ${source.length} 条`;
}
