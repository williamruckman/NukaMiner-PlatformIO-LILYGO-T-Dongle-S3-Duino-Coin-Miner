#pragma once

// =============================================================
// Web UI assets (inline JS/CSS)
//
// IMPORTANT:
//  - DO NOT EDIT these large raw strings directly in main.cpp.
//  - If you need to change UI logic, edit this file.
//  - Keep strings valid JavaScript/HTML. A single stray quote/newline
//    can break the whole web UI.
// =============================================================

#include <Arduino.h>

extern const char STATUS_JS[];

// ---- Status page JavaScript ----
// (Freeze block: treat as a single unit; avoid piecemeal edits.)
const char STATUS_JS[] PROGMEM = R"JS(

(() => {
  const el = (id) => document.getElementById(id);

  const devBox = el("dev");
  const netBox = el("net");
  const webBox = el("webui");
  const mineBox = el("mine");
  const logBox = el("log");
  const ducoLine = el("ducoLine");

  const deviceCtlBtn = el("deviceCtlBtn");
  const consoleToggleBtn = el("consoleToggleBtn");
  const followBtn = el("followBtn");

  const ducoPriceFmt = new Intl.NumberFormat("en-US", { style: "currency", currency: "USD", maximumFractionDigits: 6 });
  const ducoValueFmt = new Intl.NumberFormat("en-US", { style: "currency", currency: "USD", maximumFractionDigits: 2 });
  const ducoBalFmt = new Intl.NumberFormat("en-US", { maximumFractionDigits: 6 });
  let ducoLastFetch = 0;
  let ducoLastAttempt = 0;
  let ducoLastUser = "";
  let ducoCacheText = "";
  const DUCO_REFRESH_MS = 60000;

  function ducoSetLine(opts) {
    if (!ducoLine) return;
    const text = opts?.text ?? "";
    const html = opts?.html ?? null;
    if (!text && !html) {
      ducoLine.textContent = "";
      ducoLine.removeAttribute("title");
      ducoLine.style.display = "none";
      return;
    }
    if (html !== null) ducoLine.innerHTML = html;
    else ducoLine.textContent = text;
    if (opts?.title) ducoLine.title = opts.title;
    else ducoLine.removeAttribute("title");
    ducoLine.style.display = "block";
  }

  function ducoExtractPrice(stats) {
    if (!stats || typeof stats !== "object") return NaN;
    const keys = ["Duco price", "DUCO price", "Duco Price", "duco price", "Price"];
    for (const k of keys) {
      const v = Number(stats[k]);
      if (Number.isFinite(v)) return v;
    }
    return NaN;
  }

  async function updateDucoLine(user) {
    if (!ducoLine) return;
    const cleanUser = String(user || "").trim();
    if (!cleanUser.length) { ducoSetLine({ text: "" }); return; }

    const now = Date.now();
    if (cleanUser !== ducoLastUser) {
      ducoLastUser = cleanUser;
      ducoLastFetch = 0;
      ducoLastAttempt = 0;
      ducoCacheText = "";
    }
    if ((now - ducoLastAttempt) < DUCO_REFRESH_MS) {
      if (ducoCacheText) {
        ducoSetLine({ text: ducoCacheText });
      } else {
        ducoSetLine({ text: "DUCO: unavailable" });
      }
      return;
    }

    ducoSetLine({ text: "DUCO: loading..." });
    ducoLastAttempt = now;
    try {
      const [balRes, statsRes] = await Promise.all([
        fetch(`https://server.duinocoin.com/balances/${encodeURIComponent(cleanUser)}`, { cache: "no-store" }),
        fetch("https://server.duinocoin.com/statistics", { cache: "no-store" })
      ]);
      if (!balRes.ok || !statsRes.ok) throw new Error("duco http");
      const balJson = await balRes.json();
      const statsJson = await statsRes.json();

      const bal = Number((balJson && balJson.result) ? balJson.result.balance : balJson.balance);
      const price = ducoExtractPrice(statsJson);
      if (!Number.isFinite(bal) || !Number.isFinite(price)) throw new Error("duco data");

      const value = bal * price;
      const txt = `DUCO: ${ducoBalFmt.format(bal)} · Price: ${ducoPriceFmt.format(price)} · Value: ${ducoValueFmt.format(value)}`;
      ducoCacheText = txt;
      ducoLastFetch = now;
      ducoSetLine({ text: txt });
    } catch (e) {
      if (ducoCacheText) {
        const html = `${esc(ducoCacheText)} <span class="duco-broken" title="API currently unreachable">&#128279;</span>`;
        ducoSetLine({ html });
      } else {
        const html = `DUCO: unavailable <span class="duco-broken" title="API currently unreachable">&#128279;</span>`;
        ducoSetLine({ html });
      }
    }
  }

	  // --- Device Control modal ---
	  const dcModal = el("dcModal");
	  const dcCanvas = el("dcCanvas");
	  const dcCountdown = el("dcCountdown");
	  const dcApBtn = el("dcApBtn");
	  const dcRebootBtn = el("dcRebootBtn");
  const dcLocateBtn = el("dcLocateBtn");
	  const dcBootBtn = el("dcBootBtn");
	  const dcShotBtn = el("dcShotBtn");
	  const dcViewMain = el("dcViewMain");
  const dcViewShot = el("dcViewShot");
  const dcDl2Btn = el("dcDl2Btn");
  const dcSd2Btn = el("dcSd2Btn");
  const dcBackBtn = el("dcBackBtn");
  const dcShotMsg = el("dcShotMsg");
	  const dcCloseBtn = el("dcCloseBtn");
	  const dcMsg = el("dcMsg");
  let dcLocateOn = false;
  function dcSetLocate(on){ dcLocateOn=!!on; if(dcLocateBtn) dcLocateBtn.textContent = dcLocateOn ? "Locate: ON" : "Locate: OFF"; }

	  const DC_W = 160, DC_H = 80;
	  const dcCtx = dcCanvas ? dcCanvas.getContext("2d", { alpha: false }) : null;
	  const dcImgData = (dcCtx && dcCanvas) ? dcCtx.createImageData(DC_W, DC_H) : null;
	  const dcOut = dcImgData ? dcImgData.data : null;

	  let dcLiveTimer = null;
	  let dcCountdownTimer = null;
	  let dcNextRefreshAt = 0;
	  let dcLastPngBlob = null;
	  let dcLastPngName = null;
	  let dcEtag = "";
	  let dcLivePaused = false;

  function dcSetCountdownText(t) {
    if (!dcCountdown) return;
    dcCountdown.textContent = (t ?? "").toString();
  }

  function dcUpdateCountdown() {
    if (!dcCountdown) return;
    if (!dcNextRefreshAt) { dcSetCountdownText(""); return; }
    const ms = dcNextRefreshAt - Date.now();
    const s = Math.max(0, Math.ceil(ms / 1000));
    dcSetCountdownText(String(s));
  }

  function dcStartCountdown() {
    if (dcCountdownTimer) return;
    dcCountdownTimer = setInterval(dcUpdateCountdown, 250);
  }

  function dcStopCountdown() {
    if (dcCountdownTimer) { clearInterval(dcCountdownTimer); dcCountdownTimer = null; }
    dcNextRefreshAt = 0;
    dcSetCountdownText("");
  }

  async function setDeviceControl(enable) {
    try { await fetch(`/device_control?enable=${enable ? 1 : 0}`, { method: "POST" }); } catch (e) { /* ignore */ }
  }

  function dcSetMsg(t) {
    if (dcMsg) dcMsg.textContent = t || "";
  }

  function dcStopLive() {
    dcLivePaused = true;
    if (dcLiveTimer) { clearTimeout(dcLiveTimer); dcLiveTimer = null; }
    dcStopCountdown();
  }

  async function dcFetchAndDrawOnce() {
    if (!dcCtx || !dcCanvas || !dcImgData || !dcOut) return;
    if (dcLivePaused) return;
    try {
      const headers = {};
      if (dcEtag) headers["If-None-Match"] = dcEtag;
      const r = await fetch(`/lcd.raw?t=${Date.now()}`, { cache: "no-store", headers });
      if (r.status === 304) return;
      if (!r.ok) return;

      const et = r.headers.get("ETag");
      if (et) dcEtag = et;
      const asleep = (r.headers.get("X-LCD-Asleep") === "1");

      const buf = await r.arrayBuffer();
      const expected = DC_W * DC_H * 2;
      // Drop incomplete frames (prevents black streak tearing on WiFi hiccups).
      if (buf.byteLength !== expected) return;

      const px = new Uint16Array(buf);
      // RGB565 -> RGBA8888
      for (let i = 0, j = 0; i < px.length; i++, j += 4) {
        const v = asleep ? 0 : px[i];
        const r5 = (v >> 11) & 0x1F;
        const g6 = (v >> 5) & 0x3F;
        const b5 = v & 0x1F;
        dcOut[j + 0] = (r5 << 3) | (r5 >> 2);
        dcOut[j + 1] = (g6 << 2) | (g6 >> 4);
        dcOut[j + 2] = (b5 << 3) | (b5 >> 2);
        dcOut[j + 3] = 255;
      }
      dcCtx.putImageData(dcImgData, 0, 0);

      // If asleep, overlay a hint
      if (asleep) {
        dcCtx.save();
        dcCtx.fillStyle = "rgba(0,0,0,0.45)";
        dcCtx.fillRect(0, 0, DC_W, 16);
        dcCtx.fillStyle = "#e7eaf0";
        dcCtx.font = "10px system-ui";
        dcCtx.fillText("Display asleep", 4, 12);
        dcCtx.restore();
      }
    } catch (e) {
      // ignore transient errors
    }
  }

  function dcScheduleNext() {
    if (dcLivePaused) return;
    dcNextRefreshAt = Date.now() + 5000;
    dcStartCountdown();
    dcUpdateCountdown();
    dcLiveTimer = setTimeout(async () => {
      await dcFetchAndDrawOnce();
      dcScheduleNext();
    }, 5000);
  }

  function dcStartLive() {
    dcStopLive();
    dcLivePaused = false;
    dcEtag = "";
    // Crisp scaling
    if (dcCtx) dcCtx.imageSmoothingEnabled = false;
    if (dcCanvas) dcCanvas.style.imageRendering = "pixelated";
    // First draw immediately, then schedule the next refresh.
    dcFetchAndDrawOnce().finally(dcScheduleNext);
  }

  async function openDeviceControl() {
    if (!dcModal) return;
    dcSetMsg("");
    dcLastPngBlob = null;
    dcLastPngName = null;
    if (dcViewShot) dcViewShot.style.display = "none";
    if (dcViewMain) dcViewMain.style.display = "block";
    if (dcShotMsg) dcShotMsg.textContent = "";
    await setDeviceControl(true);
    dcModal.style.display = "block";
    dcStartLive();
  }

  async function closeDeviceControl() {
    if (!dcModal) return;
    dcStopLive();
    dcModal.style.display = "none";
    dcSetMsg("");
    if (dcShotMsg) dcShotMsg.textContent = "";
    dcLastPngBlob = null;
    dcLastPngName = null;
    await setDeviceControl(false);
  }

  if (deviceCtlBtn) deviceCtlBtn.addEventListener("click", openDeviceControl);
  if (dcCloseBtn) dcCloseBtn.addEventListener("click", closeDeviceControl);

  if (dcBootBtn) dcBootBtn.addEventListener("click", async () => {
    try { await fetch("/btn/boot", { method: "POST" }); } catch (e) { console.error("boot error", e); }
  });

  if (dcApBtn) dcApBtn.addEventListener("click", async () => {
    if (!confirm("Start AP Mode? This may change WiFi connectivity.")) return;
    try {
      dcSetMsg("Starting AP Mode...");
      await fetch("/ap/start", { method: "POST" });
      // Give the device a moment to switch modes, then refresh.
      setTimeout(() => location.reload(), 900);
    } catch (e) {
      console.error("ap start error", e);
      dcSetMsg("Failed to start AP Mode.");
    }
  });

  if (dcRebootBtn) dcRebootBtn.addEventListener("click", async () => {
    if (!confirm("Reboot device now?")) return;
    try {
      dcSetMsg("Rebooting...");
      await fetch("/reboot", { method: "POST" });
    } catch (e) {
      console.error("reboot error", e);
      dcSetMsg("Reboot failed.");
    }
  });

  if (dcLocateBtn) dcLocateBtn.addEventListener("click", async () => {
    try {
      dcSetMsg("Locating...");
      const next = dcLocateOn ? 0 : 1;
      await fetch(`/locate?enable=${next}`, { method: "POST" });
      dcSetLocate(!!next);
      dcSetMsg(dcLocateOn ? "Locate ON" : "Locate OFF");
      setTimeout(() => dcSetMsg(""), 800);
    } catch (e) {
      console.error("locate error", e);
      dcSetMsg("Locate failed.");
    }
  });


  
async function dcMakePngBlob() {
	    if (!dcCanvas) return null;
	    return await new Promise((resolve) => {
	      dcCanvas.toBlob((b) => resolve(b), "image/png");
	    });
	  }

	  if (dcShotBtn) dcShotBtn.addEventListener("click", async () => {
    try {
      dcSetMsg("Capturing screenshot...");
      // Pause only during blob creation (very brief), then resume live polling.
      const prevPaused = dcLivePaused;
      dcLivePaused = true;
      const blob = await dcMakePngBlob();
      dcLivePaused = prevPaused;

      if (!blob) throw new Error("blob");
      dcLastPngBlob = blob;
      dcLastPngName = `nukaminer_lcd_${Date.now()}.png`;

      // Show post-screenshot actions screen (download / save-to-sd).
      if (dcViewMain) dcViewMain.style.display = "none";
      if (dcViewShot) dcViewShot.style.display = "block";
      if (dcShotMsg) dcShotMsg.textContent = "Screenshot captured. Live preview will keep refreshing.";

      dcSetMsg("");
      // Ensure polling continues (in case it was paused by earlier versions).
      if (!dcLivePaused && !dcLiveTimer) dcScheduleNext();
    } catch (e) {
      console.error("screenshot error", e);
      dcSetMsg("Screenshot failed.");
      dcLivePaused = false;
      if (!dcLiveTimer) dcScheduleNext();
    }
  });


	  if (dcDl2Btn) dcDl2Btn.addEventListener("click", async () => {
    try {
      const blob = dcLastPngBlob || await dcMakePngBlob();
      if (!blob) throw new Error("blob");
      const url = URL.createObjectURL(blob);
      const a = document.createElement("a");
      a.href = url;
      a.download = dcLastPngName || `nukaminer_lcd_${Date.now()}.png`;
      document.body.appendChild(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(url), 1000);

      if (dcShotMsg) dcShotMsg.textContent = "Downloaded. Returning to Device Control...";
      setTimeout(() => {
        if (dcViewShot) dcViewShot.style.display = "none";
        if (dcViewMain) dcViewMain.style.display = "block";
        if (dcShotMsg) dcShotMsg.textContent = "";
      }, 500);
    } catch (e) {
      console.error("download error", e);
      if (dcShotMsg) dcShotMsg.textContent = "Download failed.";
    }
  });

	  if (dcSd2Btn) dcSd2Btn.addEventListener("click", async () => {
    try {
      if (dcShotMsg) dcShotMsg.textContent = "Uploading PNG to SD...";
      const blob = dcLastPngBlob || await dcMakePngBlob();
      if (!blob) throw new Error("blob");
      const fd = new FormData();
      const fname = dcLastPngName || `nukaminer_lcd_${Date.now()}.png`;
      fd.append("png", blob, fname);
      const r = await fetch("/lcd/upload_png", { method: "POST", body: fd });
      if (!r.ok) throw new Error("bad response");
      const j = await r.json().catch(() => ({}));
      if (dcShotMsg) dcShotMsg.textContent = (j && j.saved) ? `Saved to SD: ${j.saved}` : "Saved to SD.";

      setTimeout(() => {
        if (dcViewShot) dcViewShot.style.display = "none";
        if (dcViewMain) dcViewMain.style.display = "block";
        if (dcShotMsg) dcShotMsg.textContent = "";
      }, 800);
    } catch (e) {
      console.error("sd error", e);
      if (dcShotMsg) dcShotMsg.textContent = "Save to SD failed.";
    }
  });

  if (dcBackBtn) dcBackBtn.addEventListener("click", () => {
    if (dcViewShot) dcViewShot.style.display = "none";
    if (dcViewMain) dcViewMain.style.display = "block";
    if (dcShotMsg) dcShotMsg.textContent = "";
  });

// --- simple hashrate history (core1/core2/total) ---
  const hrCanvas = el("hr");
  const hrCtx = hrCanvas.getContext("2d");
  const hrHistTotal = [];
  const hrHist1 = [];
  const hrHist2 = [];
  const HR_MAX_POINTS = 120; // 10 minutes at 5s

// --- temperature history (single series) ---
  const tgCanvas = el("tg");
  const tgCtx = tgCanvas.getContext("2d");
  const tempHist = [];
  const TEMP_MAX_POINTS = 120; // 10 minutes at 5s

  let followEnabled = (localStorage.getItem("nm_follow") === "1");
  let consoleEnabled = (localStorage.getItem("nm_console") === "1");
  let lastLogSeq = 0;


  function setFollowEnabled(on) {
    followEnabled = !!on;
    localStorage.setItem("nm_follow", followEnabled ? "1" : "0");
    followBtn.textContent = `Follow: ${followEnabled ? "ON" : "OFF"}`;
  }

  function setConsoleEnabled(on) {
    consoleEnabled = !!on;
    localStorage.setItem("nm_console", consoleEnabled ? "1" : "0");
    consoleToggleBtn.textContent = `Console: ${consoleEnabled ? "ON" : "OFF"}`;
    logBox.style.display = consoleEnabled ? "block" : "none";
  }

  function esc(s) {
    return (s ?? "").toString()
      .replaceAll("&", "&amp;")
      .replaceAll("<", "&lt;")
      .replaceAll(">", "&gt;");
  }

  function fmtUptime(sec) {
    if (!Number.isFinite(sec)) return "";
    sec = Math.max(0, Math.floor(sec));
    const d = Math.floor(sec / 86400); sec %= 86400;
    const h = Math.floor(sec / 3600);  sec %= 3600;
    const m = Math.floor(sec / 60);    sec %= 60;
    const parts = [];
    if (d) parts.push(`${d}d`);
    if (h || d) parts.push(`${h}h`);
    if (m || h || d) parts.push(`${m}m`);
    parts.push(`${sec}s`);
    return parts.join(" ");
  }

  function pad2(n){ return String(n).padStart(2,"0"); }

  function fmtMDY_HMS_fromFields(mo, da, yr, hh, mm, ss) {
    return `${pad2(mo)}/${pad2(da)}/${yr}, ${pad2(hh)}:${pad2(mm)}:${pad2(ss)}`;
  }

  function fmtUtc(unix) {
    const u = Number(unix||0);
    if (!Number.isFinite(u) || u <= 0) return "-";
    const d = new Date(u * 1000);
    return fmtMDY_HMS_fromFields(
      d.getUTCMonth()+1, d.getUTCDate(), d.getUTCFullYear(),
      d.getUTCHours(), d.getUTCMinutes(), d.getUTCSeconds()
    );
  }

  function fmtLocal(unix, tz) {
    const u = Number(unix||0);
    if (!Number.isFinite(u) || u <= 0) return "-";
    const d = new Date(u * 1000);
    const zone = (tz && String(tz).trim()) ? String(tz).trim() : "";
    try {
      if (zone) {
        const parts = new Intl.DateTimeFormat("en-US", {
          timeZone: zone,
          year: "numeric", month: "2-digit", day: "2-digit",
          hour: "2-digit", minute: "2-digit", second: "2-digit",
          hour12: false
        }).formatToParts(d);
        const map = {};
        for (const p of parts) if (p.type !== "literal") map[p.type] = p.value;
        return fmtMDY_HMS_fromFields(
          Number(map.month), Number(map.day), Number(map.year),
          Number(map.hour), Number(map.minute), Number(map.second)
        );
      }
      // fall back to browser local time
      return fmtMDY_HMS_fromFields(
        d.getMonth()+1, d.getDate(), d.getFullYear(),
        d.getHours(), d.getMinutes(), d.getSeconds()
      );
    } catch (e) {
      return "-";
    }
  }

  function decodeResetReason(r) {
    const n = Number(r);
    // esp_reset_reason_t values (ESP-IDF): keep mapping simple and safe.
    // Common ones:
    //  1 POWERON, 3 SW, 4 WDT, 6 BROWNOUT, 8 PANIC
    if (!Number.isFinite(n)) return "-";
    switch (n) {
      case 1: return "Power on";
      case 3: return "Software reboot";
      case 4: return "Watchdog";
      case 5: return "Deep sleep";
      case 6: return "Brownout";
      case 8: return "Panic / abort";
      default: return `Unknown (${n})`;
    }
  }



  function pushHrSeries(arr, v) {
    if (!Number.isFinite(v)) v = 0;
    arr.push(v);
    while (arr.length > HR_MAX_POINTS) arr.shift();
  }

  function pushTempSeries(v) {
    if (!Number.isFinite(v)) v = 0;
    tempHist.push(v);
    while (tempHist.length > TEMP_MAX_POINTS) tempHist.shift();
  }

  function pushTemp(v) {
    if (!Number.isFinite(v)) return;
    tempHist.push(v);
    while (tempHist.length > TEMP_MAX_POINTS) tempHist.shift();
  }

  function drawGraph() {
    const w = hrCanvas.width, h = hrCanvas.height;
    hrCtx.clearRect(0, 0, w, h);

    // border
    hrCtx.lineWidth = 1;
    hrCtx.strokeStyle = "rgba(255,255,255,0.18)";
    hrCtx.strokeRect(0.5, 0.5, w - 1, h - 1);

    // grid
    hrCtx.strokeStyle = "rgba(255,255,255,0.08)";
    hrCtx.lineWidth = 1;
    for (let i = 1; i < 5; i++) {
      const y = (i / 5) * (h - 2) + 1;
      hrCtx.beginPath();
      hrCtx.moveTo(1, y);
      hrCtx.lineTo(w - 1, y);
      hrCtx.stroke();
    }

    const n = Math.max(hrHistTotal.length, hrHist1.length, hrHist2.length);
    if (n < 2) return;

    // scale: keep 25% headroom so max value sits ~75% up the graph
    const maxV = Math.max(
      ...hrHistTotal,
      ...hrHist1,
      ...hrHist2,
      1
    );
    const scaleMax = Math.max(1, maxV / 0.75);

    // In-graph labels (keeps mobile layout tight)
    hrCtx.save();
    hrCtx.font = "12px ui-monospace,Menlo,Consolas,monospace";
    hrCtx.textBaseline = "top";
    hrCtx.fillStyle = "rgba(255,255,255,0.70)";

    // scale labels (left)
    hrCtx.fillText(scaleMax.toFixed(1), 6, 6);
    hrCtx.textBaseline = "bottom";
    hrCtx.fillText("0.0", 6, h - 6);

    // legend (right)

    // legend (right)

    hrCtx.textBaseline = "top";

    hrCtx.textAlign = "right";

    const legendRight = w - 8; // align near the right edge, similar to the live temp indicator

    let ly = 6;

    function legendItem(label, color) {

      const boxSize = 10;

      const gap = 6;

      const tw = hrCtx.measureText(label).width;

      // place the color box just to the left of the label, keeping everything inside the canvas

      const boxX = Math.max(6, legendRight - tw - gap - boxSize);

      hrCtx.fillStyle = color;

      hrCtx.fillRect(boxX, ly + 3, boxSize, boxSize);

      hrCtx.fillStyle = "rgba(255,255,255,0.75)";

      hrCtx.fillText(label, legendRight, ly);

      ly += 16;

    }

    legendItem("Core 1", "rgba(61,255,122,0.90)");

    legendItem("Core 2", "rgba(255,209,102,0.90)");

    legendItem("Total",  "rgba(143,179,255,0.95)");

    hrCtx.textAlign = "left";

    hrCtx.restore();

    function plot(series, strokeStyle) {
      hrCtx.strokeStyle = strokeStyle;
      hrCtx.lineWidth = 2;
      hrCtx.beginPath();
      for (let i = 0; i < series.length; i++) {
        const x = (i / (series.length - 1)) * (w - 2) + 1;
        const v = Number(series[i] || 0);
        const y = (1 - (v / scaleMax)) * (h - 2) + 1;
        if (i === 0) hrCtx.moveTo(x, y);
        else hrCtx.lineTo(x, y);
      }
      hrCtx.stroke();
    }

    // Core 1 / Core 2 / Total (different colors)
    plot(hrHist1, "rgba(61,255,122,0.90)");
    plot(hrHist2, "rgba(255,209,102,0.90)");
    plot(hrHistTotal, "rgba(143,179,255,0.95)");
  }

  function drawTempGraph() {
    const w = tgCanvas.width, h = tgCanvas.height;
    tgCtx.clearRect(0, 0, w, h);

    // border
    tgCtx.lineWidth = 1;
    tgCtx.strokeStyle = "rgba(255,255,255,0.18)";
    tgCtx.strokeRect(0.5, 0.5, w - 1, h - 1);

    // grid
    tgCtx.strokeStyle = "rgba(255,255,255,0.08)";
    tgCtx.lineWidth = 1;
    for (let i = 1; i < 5; i++) {
      const y = (i / 5) * (h - 2) + 1;
      tgCtx.beginPath();
      tgCtx.moveTo(1, y);
      tgCtx.lineTo(w - 1, y);
      tgCtx.stroke();
    }

    if (tempHist.length < 2) return;

    // fixed scale (steady view)
    const scaleMin = 20.0;
    const scaleMax = 90.0;

    // in-graph labels + legend + current value
    const curV = Number(tempHist[tempHist.length - 1]);
    const curText = Number.isFinite(curV) ? (curV.toFixed(1) + "\u00B0C") : "--";

    tgCtx.save();
    tgCtx.font = "12px ui-monospace,Menlo,Consolas,monospace";
    tgCtx.textBaseline = "top";
    tgCtx.fillStyle = "rgba(255,255,255,0.70)";
    tgCtx.fillText(scaleMax.toFixed(1), 6, 6);
    tgCtx.textBaseline = "bottom";
    tgCtx.fillText(scaleMin.toFixed(1), 6, h - 6);

    tgCtx.textBaseline = "top";
    const legendX = Math.max(6, w - 150);

    // current value (top-right)
    const pad = 4;
    const boxH = 16;
    const m = tgCtx.measureText(curText);
    const boxW = m.width + pad * 2;
    const boxX = Math.max(6, w - boxW - 6);
    const boxY = 6;
    tgCtx.fillStyle = "rgba(0,0,0,0.35)";
    tgCtx.fillRect(boxX, boxY, boxW, boxH);
    tgCtx.fillStyle = "rgba(255,255,255,0.88)";
    tgCtx.fillText(curText, boxX + pad, boxY + 2);

    tgCtx.restore();

    // plot
    tgCtx.strokeStyle = "rgba(255,120,120,0.95)";
    tgCtx.lineWidth = 2;
    tgCtx.beginPath();
    for (let i = 0; i < tempHist.length; i++) {
      const x = (i / (tempHist.length - 1)) * (w - 2) + 1;
      const v = Number(tempHist[i] || 0);
      let t = (v - scaleMin) / (scaleMax - scaleMin);
      if (t < 0) t = 0;
      if (t > 1) t = 1;
      const y = (1 - t) * (h - 2) + 1;
      if (i === 0) tgCtx.moveTo(x, y);
      else tgCtx.lineTo(x, y);
    }
    tgCtx.stroke();
  }

  function estimatePowerAndEff(s) {
    // Lightweight heuristic model. Tune constants here only (no backend needed).
    // Values are "ballpark" for ESP32-S3 class boards on USB power.
    const BASE_MA = 58;             // MCU + regulators baseline (calibrated)
    const WIFI_CONNECTED_MA = 35;   // WiFi associated + idle traffic (good RSSI)
    const WEB_SESSION_MA = 8;       // extra when Web UI session is active
    const CORE_MINING_MA = 20;      // one core at full mining load (calibrated)
    const TFT_BACKLIGHT_FULL_MA = 0; // LCD backlight at 100% (no measurable delta)
    const LED_FULL_MA = 0;          // status LED at 100% (no measurable delta)

    const miningOn = !!s.mining_enabled;
    const wifiOn = !!(s.ssid && String(s.ssid).length);
    const webOn = !!s.web_session_active;

    // LCD / LED status (reported by status.json)
    const lcdOn = (s.lcd_on !== undefined) ? !!s.lcd_on : true;
    const lcdBr = Number(s.lcd_brightness);
    const lcdF = (lcdOn && Number.isFinite(lcdBr)) ? (Math.max(0, Math.min(100, lcdBr)) / 100) : 0;

    const ledOn = (s.led_on !== undefined) ? !!s.led_on : false;
    const ledBr = Number(s.led_brightness);
    const ledF = (ledOn && Number.isFinite(ledBr)) ? (Math.max(0, Math.min(100, ledBr)) / 100) : 0;

    // Per-core mining intensity: use the existing auto hash limit knobs (very low cost).
    const c1 = !!s.core1_enabled;
    const c2 = !!s.core2_enabled;
    const c1pct = Number(s.hash_limit_pct);
    const c2pct = Number(s.core2_hash_limit_pct);
    const c1f = (Number.isFinite(c1pct) ? Math.max(0, Math.min(100, c1pct)) : 100) / 100;
    const c2f = (Number.isFinite(c2pct) ? Math.max(0, Math.min(100, c2pct)) : 100) / 100;

    let ma = BASE_MA;
    // Backlight/LED scale by configured brightness.
    ma += TFT_BACKLIGHT_FULL_MA * lcdF;
    ma += LED_FULL_MA * ledF;

    // WiFi cost increases as RSSI worsens (more TX power/retries).
    if (wifiOn) {
      ma += WIFI_CONNECTED_MA;
      const rssi = Number(s.rssi);
      if (Number.isFinite(rssi)) {
        // Map RSSI: -50 => 0 extra, -80 => +12mA, -95 => +22mA
        const cl = Math.max(-95, Math.min(-50, rssi));
        const t = (cl + 50) / (-45); // 0..1 from -50..-95
        const extra = (t > 0) ? (22 * t) : 0;
        ma += extra;
      }
    }
    if (webOn) ma += WEB_SESSION_MA;

    if (miningOn) {
      if (c1) ma += CORE_MINING_MA * c1f;
      if (c2) ma += CORE_MINING_MA * c2f;
    }

    // Small bump based on hashrate to capture "real" load differences.
    const hr = Number(s.hashrate || 0); // kH/s
    const HR_SLOPE_MA_PER_KHS = 0.0;
    if (Number.isFinite(hr) && hr > 0) ma += hr * HR_SLOPE_MA_PER_KHS;

    const v = 5.0;// assumed USB supply
    const w = (ma / 1000.0) * v;
    const eff = (Number.isFinite(hr) && w > 0.0001) ? (hr / w) : 0;

    return { ma, w, eff, v };
  }

  async function tickStatus() {
    try {
      const r = await fetch("/status.json", { cache: "no-store" });
      const s = await r.json();

      updateDucoLine(s.user);

      // Device
      devBox.innerHTML = [
        `<div><b>Model:</b> ${esc(s.chip || "ESP32")}</div>`,
        `<div><b>Firmware:</b> ${esc(((s.fw_name || "NukaMiner") + " " + (s.fw_version || "") + (s.fw_channel ? (" (" + s.fw_channel + ")") : "")).trim())}</div>`,
        `<div><b>Last reset:</b> ${esc(decodeResetReason(s.reset_reason))}</div>`,
        `<div><b>Uptime:</b> ${esc(fmtUptime(Number((s.uptime_s ?? s.uptime) || 0)))}</div>`,
        `<div><b>Free heap:</b> ${esc(s.heap)} bytes${(s.heap_total ? (' (' + Math.round((Number(s.heap)||0)*100/Number(s.heap_total)) + '%)') : '')}</div>`,
        `<div><b>Temp:</b> ${(() => { const t = Number(s.temp_c); const txt = isFinite(t) ? t.toFixed(1) : '-'; let c = '#2ecc71'; if (t>=70) c='#e74c3c'; else if (t>=60) c='#e67e22'; else if (t>=50) c='#f1c40f'; return '<span style="display:inline-flex;align-items:center;gap:6px">' + esc(txt) + ' °C <span style="width:10px;height:10px;border-radius:999px;background:'+c+';display:inline-block"></span></span>'; })()}</div>`,
        `<div><b>UTC Time:</b> ${esc(fmtUtc(s.utc_unix))}</div>`,
        `<div><b>Local Time:</b> ${esc(fmtLocal(s.utc_unix, s.tz))}</div>`,
        `<div><b>Timezone:</b> ${esc(s.tz || "UTC")}</div>`
      ].join("");

      // Network
      netBox.innerHTML = [
        `<div><b>SSID:</b> ${esc(s.ssid || "-")}</div>`,
        `<div><b>RSSI:</b> ${esc(s.rssi)} dBm ${(() => {
            const r = Number(s.rssi);
            // Map RSSI to 0..4 bars: -100 (0) .. -50 (4)
            let bars = 0;
            if (isFinite(r)) {
              if (r >= -50) bars = 4;
              else if (r >= -60) bars = 3;
              else if (r >= -70) bars = 2;
              else if (r >= -80) bars = 1;
              else bars = 0;
            }
            const mk = (i) => `<span style="display:inline-block;width:5px;margin-left:2px;border-radius:2px;background:${i<=bars ? '#2f6fff' : '#223055'};height:${6 + i*3}px;vertical-align:bottom"></span>`;
            return `<span style="display:inline-flex;align-items:flex-end;margin-left:6px">${mk(1)}${mk(2)}${mk(3)}${mk(4)}</span>`;
          })()}</div>`,
        `<div><b>IP:</b> ${esc(s.ip || "-")}</div>`,
        `<div><b>SN:</b> ${esc(s.sn || "-")}</div>`,
        `<div><b>GW:</b> ${esc(s.gw || "-")}</div>`,
        `<div><b>DNS:</b> ${esc(s.dns || "-")}</div>`,
        `<div><b>NTP:</b> ${esc(s.ntp_server || "-")}</div>`,
        `<div><b>MAC:</b> ${esc(s.mac || "-")}</div>`
      ].join("");

      // Web / UI (not shown on the Status page; AP/Web state is obvious)
      webBox.innerHTML = "";
      webBox.style.display = "none";

      // Mining (include "screens" data)

      const shares = `${s.accepted ?? 0}/${s.rejected ?? ((s.shares ?? 0) - (s.accepted ?? 0))}`;
      const est = estimatePowerAndEff(s);
      mineBox.textContent =
`User: ${s.user || "-"}
Rig:  ${s.rig || "-"}
Hash: ${s.hashrate ?? "-"} ${s.hashrate_unit ?? ""}
Diff: ${s.difficulty ?? "-"}
Shares (A/R): ${shares}
Node: ${s.node || "-"}

Core 1: ${s.hashrate1 ?? "-"} ${s.hashrate_unit ?? ""}
Core 2: ${s.hashrate2 ?? "-"} ${s.hashrate_unit ?? ""}

Total: ${s.hashrate ?? (Number(s.hashrate1||0)+Number(s.hashrate2||0))} ${s.hashrate_unit ?? ""}

Est Pwr: ${Number.isFinite(est.w) ? est.w.toFixed(2) : "--"} W
Eff: ${Number.isFinite(est.eff) ? est.eff.toFixed(1) : "--"} kH/s/W`;

      // graph (core1/core2/total)
      const v1 = Number(s.hashrate1 ?? 0);
      const v2 = Number(s.hashrate2 ?? 0);
      const vt = Number(s.hashrate ?? (v1 + v2));
      pushHrSeries(hrHist1, v1);
      pushHrSeries(hrHist2, v2);
      pushHrSeries(hrHistTotal, vt);
      drawGraph();

      // Temperature graph (°C)
      pushTempSeries(Number(s.temp_c));
      drawTempGraph();

    } catch (e) {
      console.error("status.json error", e);
      const msg = (e && e.message) ? e.message : String(e);
      devBox.textContent = "Status error: " + msg;
      netBox.textContent = "Status error";
      webBox.textContent = "";
      mineBox.textContent = "Status error";
    }
  }

  async function tickLogs() {
    if (!consoleEnabled) return;
    try {
      const r = await fetch(`/logs.json?since=${lastLogSeq}`, { cache: "no-store" });
      const j = await r.json();

      if (Array.isArray(j.lines) && j.lines.length) {
        lastLogSeq = j.seq ?? lastLogSeq;
        const lines = j.lines.join("\n");
        if (lines.trim().length) {
          // append
          logBox.textContent += (logBox.textContent ? "\n" : "") + lines;
          if (followEnabled) logBox.scrollTop = logBox.scrollHeight;
        }
      }
    } catch (e) {
      console.error("logs.json error", e);
    }
  }

  // AP Mode + Reboot are in Device Control.

  consoleToggleBtn.addEventListener("click", () => setConsoleEnabled(!consoleEnabled));
  followBtn.addEventListener("click", () => setFollowEnabled(!followEnabled));

  // init
  setConsoleEnabled(consoleEnabled);
  setFollowEnabled(followEnabled);
  tickStatus();
  setInterval(tickStatus, 5000);
  setInterval(tickLogs, 2000);
})();
)JS";
