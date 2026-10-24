// Browser installer: connect, back up the whole 16 MB flash, install the bus
// board firmware, or restore a backup. Uses Espressif's esptool-js over
// Web Serial (desktop Chrome / Edge).
// esptool-js (Apache-2.0) is copied into vendor/ by the release workflow.
import { ESPLoader, Transport } from "./vendor/esptool.js";
import { md5Hex } from "./md5.js";

const FLASH_SIZE = 16 * 1024 * 1024;
const CHUNK = 0x40000;  // read 256 KB at a time
const $ = (s) => document.querySelector(s);

let port = null, transport = null, loader = null;
let busy = false, connected = false, blocked = false, backupDone = false;
let manifest = null, backupFile = null, restoreData = null;

// ---------- small UI helpers ----------
function log(t) { const el = $("#log"); el.textContent += t + "\n"; el.scrollTop = el.scrollHeight; }
const terminal = { clean() {}, writeLine: (d) => log(d), write: (d) => log(d) };
function status(id, text, kind = "") { const el = $(id); el.textContent = text; el.className = "status " + kind; }
function progress(id, frac) { const p = $(id); p.classList.remove("hide"); p.querySelector("i").style.width = Math.round(frac * 100) + "%"; }
function stamp() { const d = new Date(), z = (n) => String(n).padStart(2, "0");
  return `${d.getFullYear()}${z(d.getMonth() + 1)}${z(d.getDate())}-${z(d.getHours())}${z(d.getMinutes())}`; }
async function sha256Hex(data) {
  const h = await crypto.subtle.digest("SHA-256", data);
  return Array.from(new Uint8Array(h), (b) => b.toString(16).padStart(2, "0")).join("");
}
function download(blob, name) {
  const a = document.createElement("a"); a.href = URL.createObjectURL(blob); a.download = name;
  document.body.appendChild(a); a.click(); a.remove(); setTimeout(() => URL.revokeObjectURL(a.href), 60000);
}
function refresh() {
  const can = connected && !busy;
  $("#btnConnect").disabled = busy || connected;
  $("#btnBackup").disabled = !can || blocked;
  $("#skipBackup").disabled = !connected || busy;
  const ready = backupDone || $("#skipBackup").checked;
  $("#btnInstall").disabled = !can || blocked || !ready || !manifest;
  $("#btnPick").disabled = !can || blocked;
  $("#btnSaved").disabled = !can || blocked;
  $("#btnRestore").disabled = !can || blocked || !restoreData;
  $("#s2").classList.toggle("off", !connected);
  $("#s3").classList.toggle("off", !connected || !ready);
  $("#s4").classList.toggle("off", !connected);
}
window.addEventListener("beforeunload", (e) => { if (busy) { e.preventDefault(); e.returnValue = ""; } });

// ---------- keep a copy of the backup in this browser ----------
function idb() { return new Promise((res, rej) => { const r = indexedDB.open("note4-installer", 1);
  r.onupgradeneeded = () => r.result.createObjectStore("kv"); r.onsuccess = () => res(r.result); r.onerror = () => rej(r.error); }); }
async function idbPut(k, v) { try { const d = await idb(); await new Promise((res) => { const t = d.transaction("kv", "readwrite"); t.objectStore("kv").put(v, k); t.oncomplete = res; t.onerror = res; }); } catch (e) {} }
async function idbGet(k) { try { const d = await idb(); return await new Promise((res) => { const q = d.transaction("kv").objectStore("kv").get(k); q.onsuccess = () => res(q.result); q.onerror = () => res(undefined); }); } catch (e) { return undefined; } }

// ---------- connect ----------
async function connect() {
  busy = true; refresh(); $("#connHelp").classList.add("hide");
  try {
    status("#st1", "連接中…");
    if (!port) port = await navigator.serial.requestPort({});
    transport = new Transport(port, false);
    loader = new ESPLoader({ transport, baudrate: 921600, terminal, debugLogging: false });
    const desc = await loader.main();
    log("Chip: " + desc);
    if (loader.chip.CHIP_NAME !== "ESP32-S3") throw new Error("這不是 NOTE4（晶片：" + loader.chip.CHIP_NAME + "）");
    const id = await loader.readFlashId();
    const size = 1 << ((id >> 16) & 0xff);
    log("Flash size: " + size / 1048576 + " MB");
    if (size !== FLASH_SIZE) throw new Error("記憶體大小不符（" + size / 1048576 + "MB），這可能不是 NOTE4");

    // Locked devices (secure boot / flash encryption) cannot run this firmware.
    try {
      const sec = await loader.getSecurityInfo();
      let bits = sec.flashCryptCnt >>> 0, ones = 0; while (bits) { ones += bits & 1; bits >>>= 1; }
      const encrypted = (ones & 1) === 1, secureBoot = !!sec.parsedFlags.SECURE_BOOT_EN;
      log(`Secure boot: ${secureBoot ? "enabled" : "disabled"}, flash encryption: ${encrypted ? "enabled" : "disabled"}`);
      if (encrypted || secureBoot) {
        blocked = true;
        status("#st1", "這部 NOTE4 已被原廠鎖定（" + (secureBoot ? "安全啟動" : "快閃記憶體加密") + "），不能安裝自訂固件。請不要繼續。", "bad");
        connected = true; return;
      }
    } catch (e) { log("Security info unavailable: " + e.message); }

    connected = true;
    status("#st1", "已連接 NOTE4（ESP32-S3，16MB）。", "ok");
    $("#s1").classList.add("done");
    const saved = await idbGet("backup");
    if (saved) { $("#btnSaved").classList.remove("hide"); $("#btnSaved").textContent = "使用這部電腦保存的備份（" + saved.date + "）"; }
  } catch (e) {
    log("Connect error: " + (e && e.message));
    if (e && e.name === "NotFoundError") status("#st1", "沒有選擇裝置。", "warn");
    else status("#st1", "連接失敗：" + (e && e.message ? e.message : e), "bad");
    $("#connHelp").classList.remove("hide");
    try { await transport?.disconnect(); } catch (_) {}
    transport = null; loader = null;
  } finally { busy = false; refresh(); }
}

// ---------- backup ----------
async function backup() {
  busy = true; refresh();
  try {
    const buf = new Uint8Array(FLASH_SIZE);
    const t0 = Date.now();
    for (let addr = 0; addr < FLASH_SIZE; addr += CHUNK) {
      const part = await loader.readFlash(addr, CHUNK, (_p, got) => progress("#pg2", (addr + got) / FLASH_SIZE));
      buf.set(part.subarray(0, CHUNK), addr);
      const left = ((Date.now() - t0) / (addr + CHUNK)) * (FLASH_SIZE - addr - CHUNK) / 1000;
      status("#st2", `讀取中… ${Math.round((addr + CHUNK) / FLASH_SIZE * 100)}%（約剩 ${Math.max(0, Math.round(left))} 秒）`);
    }
    if (buf.every((b) => b === 0xff)) throw new Error("讀到的內容全部是空白，請重新連接再試");
    const sha = await sha256Hex(buf);
    const name = `note4-backup-${stamp()}.bin`;
    backupFile = { name, blob: new Blob([buf], { type: "application/octet-stream" }) };
    download(backupFile.blob, name);
    await idbPut("backup", { date: new Date().toLocaleString("zh-HK"), sha, data: backupFile.blob });
    log("Backup SHA-256: " + sha);
    backupDone = true;
    status("#st2", `備份完成：已下載 ${name}（16,384 KB）。請把這個檔案妥善保存，最好另存一份到雲端或 USB 手指。`, "ok");
    $("#redl").classList.remove("hide"); $("#s2").classList.add("done");
  } catch (e) {
    log("Backup error: " + e.message);
    status("#st2", "備份失敗：" + e.message + "。可以按「連接」重新連接後再試。", "bad");
    await lostConnection();
  } finally { busy = false; refresh(); }
}

// ---------- write (install / restore) ----------
async function writeImage(data, eraseAll, barId, stId) {
  await loader.writeFlash({
    fileArray: [{ data, address: 0 }],
    flashMode: "keep", flashFreq: "keep", flashSize: "keep",
    eraseAll, compress: true,
    reportProgress: (_i, written, total) => {
      progress(barId, written / total);
      status(stId, `寫入中… ${Math.round(written / total * 100)}%（請勿拔走 USB）`);
    },
    calculateMD5Hash: (img) => md5Hex(img),
  });
  status(stId, "正在重新啟動 NOTE4…");
  try { await loader.after(); } catch (e) { log("Reset: " + e.message); }
  try { await transport.disconnect(); } catch (_) {}
  connected = false; transport = null; loader = null;
  $("#s1").classList.remove("done");
  status("#st1", "NOTE4 已重新啟動，連接已結束。如要再操作，請重新按「連接」。");
}

async function install() {
  if (!confirm("安裝會清除 NOTE4 上現有的全部內容。確定開始？")) return;
  busy = true; refresh();
  try {
    status("#st3", "下載固件中…");
    const resp = await fetch(manifest.full_url, { cache: "no-store" });
    if (!resp.ok) throw new Error("下載固件失敗（HTTP " + resp.status + "）");
    const data = new Uint8Array(await resp.arrayBuffer());
    if ((await sha256Hex(data)) !== manifest.full_sha256) throw new Error("固件檔案校驗失敗，請重新整理此頁再試");
    log(`Firmware ${manifest.version}: ${data.length} bytes`);
    await writeImage(data, true, "#pg3", "#st3");
    status("#st3", "安裝完成！如螢幕沒有顯示「設定 Wi-Fi」，請拔出 USB 再插回。之後跟着螢幕指示用手機設定。", "ok");
    $("#s3").classList.add("done");
  } catch (e) {
    log("Install error: " + e.message);
    status("#st3", "安裝失敗：" + e.message + "。請按「連接」重新連接再試；NOTE4 不會損壞，重試即可。", "bad");
    await lostConnection();
  } finally { busy = false; refresh(); }
}

async function setRestore(blob, label) {
  const data = new Uint8Array(await blob.arrayBuffer());
  if (data.length !== FLASH_SIZE) {
    restoreData = null;
    $("#restoreInfo").textContent = `檔案大小不對（${Math.round(data.length / 1024)} KB），備份檔應為 16,384 KB。`;
  } else {
    restoreData = data;
    $("#restoreInfo").textContent = "已選擇：" + label;
  }
  refresh();
}

async function restore() {
  if (!confirm("還原會用備份檔覆蓋 NOTE4 上的全部內容。確定開始？")) return;
  busy = true; refresh();
  try {
    await writeImage(restoreData, false, "#pg4", "#st4");
    status("#st4", "還原完成！拔出 USB 再插回，NOTE4 會回到備份時的狀態。", "ok");
    $("#s4").classList.add("done");
  } catch (e) {
    log("Restore error: " + e.message);
    status("#st4", "還原失敗：" + e.message + "。請重新連接再試。", "bad");
    await lostConnection();
  } finally { busy = false; refresh(); }
}

async function lostConnection() {
  try { await transport?.disconnect(); } catch (_) {}
  connected = false; transport = null; loader = null;
  status("#st1", "連接已中斷，請按「連接」重新連接。", "warn");
}

// ---------- wiring ----------
if (!("serial" in navigator)) {
  $("#noSerial").classList.remove("hide");
  $("#btnConnect").disabled = true;
} else {
  navigator.serial.addEventListener("disconnect", () => {
    if (connected && !busy) { connected = false; transport = null; loader = null; status("#st1", "NOTE4 已拔除。", "warn"); refresh(); }
  });
  $("#btnConnect").onclick = connect;
  $("#btnBackup").onclick = backup;
  $("#btnInstall").onclick = install;
  $("#btnRestore").onclick = restore;
  $("#btnRedownload").onclick = () => backupFile && download(backupFile.blob, backupFile.name);
  $("#skipBackup").onchange = refresh;
  $("#btnPick").onclick = () => $("#fileRestore").click();
  $("#fileRestore").onchange = (e) => { const f = e.target.files[0]; if (f) setRestore(f, f.name); };
  $("#btnSaved").onclick = async () => { const s = await idbGet("backup"); if (s) setRestore(s.data, "這部電腦保存的備份（" + s.date + "）"); };
  refresh();
}

fetch("firmware/ota.json", { cache: "no-store" }).then((r) => r.json()).then((m) => {
  manifest = { ...m, full_url: new URL(m.full_url, location.href).href };
  $("#ver").textContent = m.version; refresh();
}).catch(() => { $("#ver").textContent = "（未能讀取固件資料）"; });
