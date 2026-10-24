import { getHistory } from "/assets/data.js";

export function bindHistoryPanel() {
  const panel = document.getElementById("history-panel");
  const target = document.getElementById("history-list");
  panel.addEventListener("toggle", async () => {
    if (!panel.open) return;
    target.replaceChildren(Object.assign(document.createElement("p"), {
      className: "small-note", textContent: "正在读取历史摘要…",
    }));
    try {
      const items = await getHistory();
      const fragment = document.createDocumentFragment();
      for (const item of items) {
        const row = document.createElement("div");
        row.className = "history-item";
        row.textContent = `会话 #${item.session_id} · 修订 ${item.revision} · ${item.conclusion} · ${item.capture_integrity} · CRC32 ${item.content_crc32}`;
        fragment.append(row);
      }
      if (!items.length) {
        const empty = document.createElement("p");
        empty.className = "small-note";
        empty.textContent = "尚无历史会话摘要。";
        fragment.append(empty);
      }
      target.replaceChildren(fragment);
    } catch (error) {
      const message = document.createElement("p");
      message.className = "small-note";
      message.textContent = `历史摘要读取失败：${error.message}`;
      target.replaceChildren(message);
    }
  });
}
