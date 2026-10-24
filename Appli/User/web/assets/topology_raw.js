import { crc32, fetchBytes } from "./data.js";
import {
  UsbTopologyChangedError, manifestSignature, number, topologyJson,
  topologyRequest, validManifest,
} from "./topology.js";

const PAGE_SIZE = 1200;

export async function readConfigRaw(config, expectedManifest, signal) {
  const before = await topologyJson("/api/usb/manifest", signal);
  if (!validManifest(before) || manifestSignature(before) !== manifestSignature(expectedManifest)) {
    throw new UsbTopologyChangedError("配置读取前 USB 代次已变化");
  }
  const total = number(expectedManifest.config_captured_bytes);
  if (total <= 0 || total > 65535) throw new Error("配置捕获总长度无效");
  const allBytes = new Uint8Array(total);
  for (const current of expectedManifest.configurations || []) {
    const length = number(current.captured_length);
    const index = number(current.index);
    const base = number(current.raw_offset);
    if (length <= 0 || base < 0 || base + length > total) throw new Error("配置捕获范围无效");
    for (let localOffset = 0; localOffset < length;) {
      const page = await topologyRequest(
        `/api/usb/config/page/${index}/${localOffset}`, fetchBytes, signal);
      const expected = Math.min(PAGE_SIZE, length - localOffset);
      if (page.length !== expected) throw new Error(`分页长度异常，偏移 ${localOffset}`);
      allBytes.set(page, base + localOffset);
      localOffset += page.length;
    }
  }
  const after = await topologyJson("/api/usb/manifest", signal);
  if (!validManifest(after) || manifestSignature(after) !== manifestSignature(expectedManifest)) {
    throw new UsbTopologyChangedError("配置读取期间 USB 代次已变化");
  }
  const expectedCrc = String(expectedManifest.config_crc32 || "").replace(/^0x/i, "").toUpperCase();
  if (!expectedCrc || crc32(allBytes) !== expectedCrc) throw new Error("配置描述符 CRC32 不一致");
  const start = number(config.raw_offset);
  const end = start + number(config.captured_length);
  return allBytes.slice(start, end);
}
