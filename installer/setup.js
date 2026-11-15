// decidio installer, step 4: set up a board that runs decidio firmware, over its serial console, with Web Serial.
//
// The console (firmware/main/console.c) reads text lines on the chip's own USB port (XIAO: USB-Serial-JTAG, the baud
// rate is ignored) and on UART0 at 921600 baud (ESP32-S3-CAM: the socket marked UART, through a USB-UART chip).
// Arguments are words or "double-quoted strings"; a line holds at most 255 bytes. Each command answers with a JSON
// line, mostly {"ok":"<cmd>"} or {"error":"..."}. Log lines, decisions ({"answers":..}) and events arrive in between,
// so each command waits for the one key it needs, with a timeout, and only one command is in flight at a time.
//
//   info                         -> {"info":{...}}                      board summary
//   wifi "<ssid>" "<password>"   -> {"ok":"wifi"} ({"error":"wifi: .."} first when refused), then `wifi` is polled
//   wifi                         -> {"wifi":{"state":..,"setup_open":..,"ip":..,"host":..,"rssi":..,"channel":..,"reason":..}}
//   alert set <key> "<value>"    -> {"ok":"alert"} or {"error":".."}    an empty value removes the key (store.c)
//   alert test telegram|webhook|mqtt -> {"alert_test":{"ok":..,"status":..,"ms":..,"detail":".."},..}  (up to 30 s)
//   reboot                       -> no answer; the board starts again and prints {"info":..} when it is ready
//
// Nothing typed here is stored by the page (no localStorage, no server). Secrets are masked in the console log.

const $ = id => document.getElementById(id);
const el = (tag, props, ...kids) => { const e = Object.assign(document.createElement(tag), props || {}); e.append(...kids); return e; };
const sleep = ms => new Promise(r => setTimeout(r, ms));
const enc = new TextEncoder();
const bytes = s => enc.encode(s).length;
const LINE_MAX = 255;                                   // console.c: char line[256]

class LinkError extends Error {
  constructor(code, message) { super(message || code); this.code = code; }
}

// matchers: given a parsed JSON line, return the answer or undefined
const want = {
  info: o => (o.info && typeof o.info === "object" ? o.info : undefined),
  wifi: o => (o.wifi && typeof o.wifi === "object" && "setup_open" in o.wifi ? o.wifi : undefined),
  test: o => (o.alert_test && typeof o.alert_test === "object" ? o.alert_test : undefined),
  done: cmd => o => (o.ok === cmd ? { ok: true } : typeof o.error === "string" ? { ok: false, error: o.error } : undefined),
};

// ---- the serial link ---------------------------------------------------------------------------------------------------
class BoardLink {
  constructor(port, hooks) {
    this.port = port;
    this.hooks = hooks;                                 // line(text, kind), json(obj), lost()
    this.waiters = new Set();
    this.chain = Promise.resolve();
    this.secrets = new Set();
    this.isOpen = false;
    this.partial = "";
  }

  async open() {
    await this.port.open({ baudRate: 921600, bufferSize: 16384 });
    this.isOpen = true;
    // Opening must not reset the board or hold it in download mode (decidio/serialio.py opens with dtr = rts = False).
    // RTS goes first: on the usual two-transistor circuit of a USB-UART board, RTS alone pulls EN low (reset) and DTR
    // alone pulls IO0 low, which does nothing while the chip runs. So the step between the two calls is harmless.
    try {
      await this.port.setSignals({ requestToSend: false });
      await this.port.setSignals({ dataTerminalReady: false });
    } catch (e) {
      this.hooks.line("The page could not set the DTR and RTS signals: " + (e.message || e.name), "sys");
    }
    this.decoder = new TextDecoder();
    this.reading = this.readLoop();
  }

  async readLoop() {
    while (this.isOpen && this.port.readable) {
      let reader;
      try { reader = this.port.readable.getReader(); } catch (e) { break; }
      this.reader = reader;
      try {
        for (;;) {
          const { value, done } = await reader.read();
          if (done) break;
          if (value) this.feed(this.decoder.decode(value, { stream: true }));
        }
      } catch (e) {
        if (this.isOpen) this.hooks.line("Read error: " + (e.message || e.name), "sys");   // a break or framing error: go on
      } finally {
        try { reader.releaseLock(); } catch (e) { /* already released */ }
        this.reader = null;
      }
    }
    if (this.isOpen) {                                  // the device went away (cable, or a board that re-enumerates)
      this.isOpen = false;
      this.fail(new LinkError("lost"));
      try { await this.port.close(); } catch (e) { /* already gone */ }
      this.hooks.lost(this);
    }
  }

  feed(text) {
    this.partial += text;
    let i;
    while ((i = this.partial.search(/[\r\n]/)) >= 0) {
      const line = this.partial.slice(0, i);
      this.partial = this.partial.slice(i + 1);
      if (line) this.onLine(line);
    }
    if (this.partial.length > 20000) {                  // no line end in a long time: show what came
      this.onLine(this.partial);
      this.partial = "";
    }
  }

  onLine(raw) {
    const line = raw.replace(/\x1b\[[0-9;]*[A-Za-z]/g, "").replace(/[\x00-\x08\x0b-\x1f\x7f]/g, "");
    if (!line.trim()) return;
    let obj = null;
    if (line[0] === "{") {
      try { obj = JSON.parse(line); } catch (e) { obj = null; }
      if (obj && typeof obj !== "object") obj = null;
    }
    const kind = obj && typeof obj.error === "string" ? "err" : obj && obj.answers ? "answers" : "in";
    this.hooks.line(this.mask(line), kind);
    if (!obj) return;
    for (const w of [...this.waiters]) {
      let r;
      try { r = w.match(obj); } catch (e) { r = undefined; }
      if (r !== undefined) {
        this.waiters.delete(w);
        clearTimeout(w.timer);
        w.resolve(r);
      }
    }
    this.hooks.json(obj);
  }

  // a promise for the next line that `match` accepts; .cancel() stops waiting
  waitFor(match, ms) {
    let w;
    const promise = new Promise((resolve, reject) => { w = { match, resolve, reject }; });
    w.timer = setTimeout(() => { this.waiters.delete(w); w.reject(new LinkError("timeout")); }, ms);
    this.waiters.add(w);
    promise.cancel = () => { clearTimeout(w.timer); this.waiters.delete(w); };
    promise.catch(() => {});
    return promise;
  }

  fail(err) {
    for (const w of this.waiters) { clearTimeout(w.timer); w.reject(err); }
    this.waiters.clear();
  }

  async write(line, shown) {
    if (!this.isOpen || !this.port.writable) throw new LinkError("closed");
    if (bytes(line) > LINE_MAX) throw new LinkError("long", "The command is longer than " + LINE_MAX + " bytes.");
    this.hooks.line("> " + (shown != null ? shown : this.mask(line)), "out");
    const w = this.port.writable.getWriter();
    try { await w.write(enc.encode(line + "\n")); } finally { w.releaseLock(); }
  }

  // one command at a time: the next one goes out after the answer (or the timeout) of the one before
  request(line, match, ms, shown) {
    const job = this.chain.then(async () => {
      const p = this.waitFor(match, ms);
      try { await this.write(line, shown); } catch (e) { p.cancel(); throw e; }
      return p;
    });
    this.chain = job.catch(() => {});
    return job;
  }

  send(line, shown) {
    const job = this.chain.then(() => this.write(line, shown));
    this.chain = job.catch(() => {});
    return job;
  }

  secret(value) { if (value) this.secrets.add(value); }

  mask(text) {
    let out = text;
    for (const s of this.secrets) if (s.length >= 4) out = out.split(s).join("[hidden]");
    return out;
  }

  async close() {
    if (!this.isOpen) return;
    this.isOpen = false;
    this.fail(new LinkError("closed"));
    try { if (this.reader) await this.reader.cancel(); } catch (e) { /* closed */ }
    try { await this.reading; } catch (e) { /* closed */ }
    try { await this.port.close(); } catch (e) { /* closed */ }
  }
}

// ---- the alert fields (alert.c s_cfg and web.c h_alerts give the lengths) ---------------------------------------------
const FIELDS = [
  { key: "tg_token", dest: "telegram", label: "Telegram bot token", max: 63, secret: true, trim: true,
    re: /^\d+:[A-Za-z0-9_-]+$/, form: "The bot token has the form 123456789:ABCdef, with no spaces." },
  { key: "tg_chat", dest: "telegram", label: "Telegram chat ID", max: 23, trim: true,
    re: /^(-?\d+|@[A-Za-z0-9_]{4,})$/, form: "The chat ID is a number, for example 123456789 or -1001234567890." },
  { key: "hook_url", dest: "webhook", label: "Webhook address", max: 199, secret: true, trim: true,
    re: /^https?:\/\/\S+$/i, form: "The webhook address starts with http:// or https:// and has no spaces." },
  { key: "hook_fmt", dest: "webhook", label: "Webhook format", max: 7, select: true },
  { key: "hook_hdr", dest: "webhook", label: "Webhook header", max: 159, secret: true, trim: true,
    re: /^[!#$%&'*+.^_`|~0-9A-Za-z-]{1,47}:[ \t]*\S/, form: "The header has the form Name: value, for example Authorization: Bearer abc123." },
  { key: "mqtt_url", dest: "mqtt", label: "MQTT broker address", max: 127, secret: true, trim: true,
    re: /^mqtts?:\/\/\S+$/i, form: "The broker address starts with mqtt:// or mqtts:// and has no spaces." },
  { key: "mqtt_user", dest: "mqtt", label: "MQTT user", max: 47 },
  { key: "mqtt_pass", dest: "mqtt", label: "MQTT password", max: 63, secret: true },
];
const DESTS = { telegram: "Telegram", webhook: "Webhook", mqtt: "MQTT" };

const BOARD_EXTRA = {
  "s3cam-lcd": "ESP32-S3-CAM with a display connector (camera, no microphone)",
  unknown: "Unknown board",
};

// a text value that the console can carry: no quote or backslash (they end or break the argument), no control
// characters, and not too long
function badText(value, label, max) {
  if (/["\\]/.test(value)) return label + ": do not use the characters \" and \\.";
  if (/[\x00-\x1f\x7f]/.test(value)) return label + ": do not use control characters.";
  if (bytes(value) > max) return label + ": use " + max + " characters or fewer.";
  return "";
}

// ---- the page ----------------------------------------------------------------------------------------------------------
export function initSetup(opts) {
  start(opts).catch(e => {
    const m = $("connMsg");
    m.textContent = "Step 4 did not start: " + (e.message || e);
    m.className = "msg bad";
  });
}

async function start({ demo, mobile, boardName, version }) {
  let serial = null;
  if (demo) {
    serial = (await import("./demo.js")).createDemoSerial();
  } else if ("serial" in navigator && window.isSecureContext && !mobile) {
    serial = navigator.serial;
  }

  let link = null, info = null, wifi = null, restarting = false, connecting = false, wifiSaving = false;
  const forms = $("setupForms");

  // ---- messages ----
  function say(target, text, kind) {
    const m = typeof target === "string" ? $(target) : target;
    m.textContent = text;
    m.className = (m.classList.contains("result") ? "msg result" : "msg") + (kind ? " " + kind : "");
  }
  function busy(btn, on) {
    btn.disabled = on;
    if (on) btn.setAttribute("aria-busy", "true"); else btn.removeAttribute("aria-busy");
  }
  function invalid(input, msgId, text) {
    input.setAttribute("aria-invalid", "true");
    say(msgId, text, "bad");
    input.focus();
  }
  document.querySelectorAll("#setupForms input").forEach(i => i.addEventListener("input", () => i.removeAttribute("aria-invalid")));

  // ---- console log ----
  const LOG_MAX = 500;
  const logLines = [];
  const logBox = $("log");
  let showAnswers = false, hiddenAnswers = 0;
  function stamp() { return new Date().toTimeString().slice(0, 8); }
  function logAdd(text, kind) {
    if (kind === "answers" && !showAnswers) { hiddenAnswers++; return; }
    const entry = { text: stamp() + " " + (text.length > 4000 ? text.slice(0, 4000) + " ..." : text), kind };
    logLines.push(entry);
    if (logLines.length > LOG_MAX) logLines.shift();
    const near = logBox.scrollHeight - logBox.scrollTop - logBox.clientHeight < 40;
    logBox.append(el("div", { className: kind, textContent: entry.text }));
    while (logBox.childElementCount > LOG_MAX) logBox.firstElementChild.remove();
    if (near) logBox.scrollTop = logBox.scrollHeight;
  }
  $("logAnswers").addEventListener("change", e => {
    showAnswers = e.target.checked;
    logAdd(showAnswers ? "The log shows the answers of the model." : "The log hides the answers of the model.", "sys");
  });
  $("logClear").addEventListener("click", () => {
    logLines.length = 0;
    logBox.replaceChildren();
    hiddenAnswers = 0;
    say("logMsg", "The log is clear.", "");
  });
  $("logCopy").addEventListener("click", async () => {
    const text = logLines.map(l => l.text).join("\n") + (hiddenAnswers ? "\n(" + hiddenAnswers + " lines with answers of the model are not shown)" : "");
    try {
      await navigator.clipboard.writeText(text);
      say("logMsg", "The log is on the clipboard.", "ok");
    } catch (e) {
      const r = document.createRange();
      r.selectNodeContents(logBox);
      const s = getSelection();
      s.removeAllRanges();
      s.addRange(r);
      say("logMsg", "Press Ctrl+C to copy the selected log.", "");
    }
  });

  // ---- connection state ----
  function setState(on, text) {
    const s = $("connState");
    s.textContent = text;
    s.classList.toggle("on", on);
  }
  function setConnected(on) {
    $("connect").hidden = on;
    $("disconnect").hidden = !on;
    if (!on) {
      forms.disabled = true;
      setState(false, "Not connected");
      for (const id of ["wifiPass", "pagePass"]) $(id).value = "";
      for (const f of FIELDS) { const i = $(f.key); if (f.secret) { i.value = ""; f.dirty = false; } }
    }
  }
  function ready() {
    forms.disabled = false;
    setState(true, demo ? "Connected to the demo board" : "Connected");
  }

  const hooks = {
    line: logAdd,
    json(o) {
      if (o.wifi && typeof o.wifi === "object" && typeof o.wifi.state === "string") {
        mergeWifi(o.wifi);
        if (!wifiSaving && !forms.disabled) say("wifiMsg", wifiText(), wifi.state === "online" ? "ok" : "");   // e.g. online after a restart
      }
    },
    lost(l) {
      if (l !== link) return;
      if (restarting) return;                           // the restart waits for the port itself
      link = null;
      setConnected(false);
      say("connMsg", "The connection to the board stopped. Connect the USB cable. Then click Connect to the board.", "bad");
    },
  };

  if (!serial) {
    $("connect").disabled = true;
    const u = $("serialUnsupported");
    u.textContent = mobile ? "A phone cannot set up a board over USB. Use Chrome or Edge on a computer."
      : !window.isSecureContext ? "Open this page over https:// or from localhost to use USB."
      : "This browser cannot connect to USB devices. Use Chrome or Edge on a computer.";
    u.hidden = false;
  }

  // ---- connect ----
  async function waitReady(l, total) {
    const end = Date.now() + total;
    for (;;) {
      try {
        return await l.request("info", want.info, 4000);
      } catch (e) {
        if (e.code !== "timeout" || Date.now() > end) throw e;
      }
    }
  }

  async function openLink(port) {
    const l = new BoardLink(port, hooks);
    try {
      await l.open();
    } catch (e) {
      say("connMsg", "The page cannot open the port. Close the install window and each other program that uses the port. Then try again. (" + (e.message || e.name) + ")", "bad");
      return null;
    }
    link = l;
    logAdd(demo ? "The port of the demo board is open." : "The port is open.", "sys");
    setConnected(true);
    setState(false, "Waiting for the board");
    say("connMsg", "The port is open. The page waits for the board to answer.", "busy");
    try {
      info = await waitReady(l, 30000);
    } catch (e) {
      if (l !== link) return null;
      await l.close();
      link = null;
      setConnected(false);
      say("connMsg", e.code === "timeout"
        ? "The board does not answer. Make sure that the board has decidio firmware: install an idea in step 3. A board in download mode does not answer: disconnect the USB cable and connect it again."
        : "The connection stopped before the board answered. Connect the USB cable and try again.", "bad");
      $("logBox").open = true;
      return null;
    }
    renderSummary();
    await refreshWifi(l);
    ready();
    return l;
  }

  $("connect").addEventListener("click", async () => {
    if (!serial || connecting) return;
    connecting = true;
    busy($("connect"), true);
    say("connMsg", "", "");
    try {
      let port;
      try {
        port = await serial.requestPort();
      } catch (e) {
        say("connMsg", e.name === "NotFoundError" ? "No port was selected. Click Connect to the board and select the port of the board."
          : "The browser did not give a port: " + (e.message || e.name), "bad");
        return;
      }
      const l = await openLink(port);
      if (l) say("connMsg", "Connected. The board answers.", "ok");
    } finally {
      connecting = false;
      busy($("connect"), false);
    }
  });

  $("disconnect").addEventListener("click", async () => {
    const l = link;
    link = null;
    if (l) await l.close();
    setConnected(false);
    logAdd("The port is closed.", "sys");
    say("connMsg", "Disconnected. Other programs can use the port now.", "");
  });

  window.addEventListener("pagehide", () => { if (link) link.close(); });

  // ---- board summary ----
  function row(dt, dd) { return [el("dt", { textContent: dt }), el("dd", dd)]; }
  function renderSummary() {
    if (!info) return;
    const s = info.sensors && typeof info.sensors === "object" ? info.sensors : {};
    const id = String(info.board || s.profile || "unknown");
    const name = boardName(id) || BOARD_EXTRA[id];
    const rows = [];
    rows.push(row("Board", { textContent: name ? name + " (" + id + ")" : id }));
    const cam = "camera" in s ? s.camera : (info.camera && info.camera !== "none" ? info.camera : null);
    rows.push(row("Camera", { textContent: cam ? String(cam) : "Not found" }));
    rows.push(row("Microphone", { textContent: info.mic ? "Yes" : "No" }));
    if ("card" in s) rows.push(row("Memory card", { textContent: s.card ? "Yes" : "No" }));
    const m = info.model;
    if (!m || typeof m !== "object") {
      rows.push(row("Model", { textContent: "No model. Install an idea in step 3." }));
    } else {
      const q = Array.isArray(m.questions) ? m.questions.length : 0, na = Array.isArray(m.na) ? m.na.length : 0;
      const uses = [m.image ? "camera" : "", m.audio ? "sound" : ""].filter(Boolean).join(" and ");
      rows.push(row("Model", { textContent: String(m.name || "unnamed") + ": " + q + " questions" + (uses ? ", uses " + uses : "") +
        (na ? ". " + na + " questions are off: the board has no sensor for them." : "") }));
    }
    const fw = String(info.fw || "").replace(/^decidio-rt\s*/, "") || "unknown";
    const v = version();
    rows.push(row("Firmware", { textContent: fw + (v && fw !== v ? ". This page installs " + v + "." : "") }));
    if (s.psram_mb != null) rows.push(row("Memory", { textContent: "PSRAM " + s.psram_mb + " MB, flash " + s.flash_mb + " MB" }));
    if (Array.isArray(info.pins)) rows.push(row("Free pins for rules", { textContent: info.pins.length ? "GPIO " + info.pins.join(", ") : "None" }));
    rows.push(row("Wi-Fi", { id: "sumWifi", textContent: wifiShort() }));
    $("summary").replaceChildren(...rows.flat());
    $("summary").hidden = false;
    $("summaryEmpty").hidden = true;
  }

  // ---- Wi-Fi ----
  const goodHost = h => typeof h === "string" && /^[A-Za-z0-9-]{1,63}$/.test(h);
  const goodIp = ip => typeof ip === "string" && /^\d{1,3}(\.\d{1,3}){3}$/.test(ip) && ip !== "0.0.0.0";

  function wifiShort() {
    if (!wifi) return "Unknown";
    const host = goodHost(wifi.host) ? wifi.host : "";
    switch (wifi.state) {
      case "online": return "Online" + (goodIp(wifi.ip) ? ", " + wifi.ip : "") + (host ? ", name " + host : "");
      case "connecting": return "Connecting" + (host ? ", name " + host : "");
      case "setup": return "Not set up. Setup network " + (host || "decidio-xxxx") + (wifi.setup_open === false ? " is closed" : " is open");
      case "off": return "Off";
      default: return String(wifi.state);
    }
  }

  function reasonHint(r) {
    if (!r) return "";
    if (r === 15 || r === 202 || r === 204) return " The password is probably wrong.";
    if (r === 201) return " The board does not find the network. Use a 2.4 GHz network. Move the board nearer to the router.";
    return " Reason code " + r + ".";
  }

  function wifiText() {
    if (!wifi) return "";
    switch (wifi.state) {
      case "online": return "The board is online." + (wifi.rssi ? " Signal " + wifi.rssi + " dBm, channel " + wifi.channel + "." : "");
      case "connecting": return "The board tries to connect to the network." + reasonHint(wifi.reason);
      case "setup": return "The board has no Wi-Fi network. Type a network name and a password, then click Save Wi-Fi.";
      case "off": return "Wi-Fi is off on the board. Save a network to start Wi-Fi.";
      default: return "Wi-Fi state: " + wifi.state + ".";
    }
  }

  function mergeWifi(w) {
    const next = Object.assign({}, wifi || {}, w);
    if (!goodHost(next.host) && typeof w.open === "string") {            // event lines carry "open":"http://<host>.local"
      const m = /^http:\/\/([A-Za-z0-9-]{1,63})\.local$/.exec(w.open);
      if (m) next.host = m[1];
    }
    if (!goodHost(next.host) && goodHost(w.join)) next.host = w.join;
    if (next.state !== "online") { next.ip = ""; next.rssi = 0; next.channel = 0; }
    wifi = next;
    renderWifi();
  }

  function renderWifi() {
    const on = wifi && wifi.state === "online" && goodIp(wifi.ip);
    $("wifiAddr").hidden = !on;
    if (on) {
      const ipUrl = "http://" + wifi.ip + "/";
      $("addrIp").href = ipUrl;
      $("addrIp").textContent = "http://" + wifi.ip;
      $("openPage").href = ipUrl;
      if (goodHost(wifi.host)) {
        $("addrHost").href = "http://" + wifi.host + ".local/";
        $("addrHost").textContent = "http://" + wifi.host + ".local";
        $("addrHost").parentElement.hidden = false;
      } else {
        $("addrHost").parentElement.hidden = true;
      }
    }
    if (wifi && goodHost(wifi.host)) document.querySelectorAll(".hostName").forEach(s => { s.textContent = wifi.host; });
    const sum = $("sumWifi");
    if (sum) sum.textContent = wifiShort();
  }

  async function refreshWifi(l) {
    try {
      const st = await l.request("wifi", want.wifi, 3000);
      mergeWifi(st);
      say("wifiMsg", wifiText(), wifi.state === "online" ? "ok" : "");
    } catch (e) {
      if (e.code === "timeout") say("wifiMsg", "The board did not tell its Wi-Fi state. Its firmware is possibly older than 0.2.", "bad");
    }
  }

  $("wifiShow").addEventListener("change", e => { $("wifiPass").type = e.target.checked ? "text" : "password"; });
  $("pageShow").addEventListener("change", e => { $("pagePass").type = e.target.checked ? "text" : "password"; });

  $("wifiForm").addEventListener("submit", async ev => {
    ev.preventDefault();
    const l = link;
    if (!l) return;
    const ssidIn = $("wifiSsid"), passIn = $("wifiPass");
    const ssid = ssidIn.value, pass = passIn.value;
    if (!ssid) return invalid(ssidIn, "wifiMsg", "Type the name of the network.");
    if (/["\\]/.test(ssid + pass)) {
      return invalid(/["\\]/.test(ssid) ? ssidIn : passIn, "wifiMsg",
        "The page cannot send a name or a password with the character \" or \\. Use the setup network of the board for this network.");
    }
    if (/[\x00-\x1f\x7f]/.test(ssid)) return invalid(ssidIn, "wifiMsg", "The network name has control characters. Type it again.");
    if (/[\x00-\x1f\x7f]/.test(pass)) return invalid(passIn, "wifiMsg", "The password has control characters. Type it again.");
    if (bytes(ssid) > 32) return invalid(ssidIn, "wifiMsg", "The network name is longer than 32 bytes. A Wi-Fi network name cannot be longer.");
    // WPA: a passphrase of 8 to 63 characters, or the key as 64 hex digits (wifi.c rt_wifi_set allows up to 64 bytes)
    if (pass && !(bytes(pass) >= 8 && bytes(pass) <= 63) && !/^[0-9A-Fa-f]{64}$/.test(pass)) {
      return invalid(passIn, "wifiMsg", "The password must have 8 to 63 characters. Leave it empty for an open network.");
    }
    const btn = $("saveWifi");
    busy(btn, true);
    wifiSaving = true;
    l.secret(pass);
    say("wifiMsg", "The board connects to the network. This can take 30 seconds.", "busy");
    try {
      const r = await l.request('wifi "' + ssid + '" "' + pass + '"', want.done("wifi"), 10000,
        'wifi "' + ssid + '" "' + (pass ? "[hidden]" : "") + '"');
      if (!r.ok) {
        say("wifiMsg", "The board did not accept the network: " + r.error + ".", "bad");
        return;
      }
      passIn.value = "";
      // the same sequence as `decidio wifi` (decidio/serialio.py): ask for the state every 2 s, 15 times
      let st = null;
      for (let i = 0; i < 15 && l === link; i++) {
        await sleep(2000);
        if (l !== link) return;
        try { st = await l.request("wifi", want.wifi, 3000); } catch (e) { if (e.code !== "timeout") throw e; continue; }
        mergeWifi(st);
        if (st.state === "online") break;
        say("wifiMsg", "The board tries to connect to the network." + reasonHint(st.reason) + " (" + (i + 1) * 2 + " s)", "busy");
      }
      if (wifi && wifi.state === "online") {
        say("wifiMsg", wifiText() + " Use the address below to open the page of the board.", "ok");
      } else {
        say("wifiMsg", "The board did not connect in 30 seconds." + reasonHint(wifi && wifi.reason) +
          " Make sure that the network name and the password are correct. The board continues to try.", "bad");
      }
    } catch (e) {
      say("wifiMsg", e.code === "timeout" ? "The board did not answer." : "The command did not go to the board: " + e.message, "bad");
    } finally {
      wifiSaving = false;
      busy(btn, false);
    }
  });

  // ---- page password ----
  $("passForm").addEventListener("submit", async ev => {
    ev.preventDefault();
    const l = link;
    if (!l) return;
    const input = $("pagePass"), p = input.value;
    const bad = badText(p, "Page password", 60);
    if (bad) return invalid(input, "passMsg", bad);
    const btn = $("savePass");
    busy(btn, true);
    l.secret(p);
    say("passMsg", "The page sends the password to the board.", "busy");
    try {
      // the store key of the page password (web.c h_password); web.c load_auth() reads it only when the web server starts
      const r = await l.request('alert set page_pass "' + p + '"', want.done("alert"), 5000,
        'alert set page_pass "' + (p ? "[hidden]" : "") + '"');
      if (!r.ok) return say("passMsg", "The board did not save the password: " + r.error + ".", "bad");
      input.value = "";
      say("passMsg", p
        ? "Saved. The board uses the new password after it restarts. Click Restart the board."
        : "Removed. The page of the board has no password after it restarts. Click Restart the board.", "ok");
    } catch (e) {
      say("passMsg", e.code === "timeout" ? "The board did not answer." : "The command did not go to the board: " + e.message, "bad");
    } finally {
      busy(btn, false);
    }
  });

  // ---- alerts ----
  for (const f of FIELDS) {
    f.dirty = false;
    $(f.key).addEventListener(f.select ? "change" : "input", () => { f.dirty = true; });
  }
  const removeBox = dest => document.querySelector('[data-remove="' + dest + '"]');
  const valueOf = f => (f.trim ? $(f.key).value.trim() : $(f.key).value);
  document.querySelectorAll("[data-remove]").forEach(box => box.addEventListener("change", () => {
    for (const f of FIELDS) if (f.dest === box.dataset.remove) $(f.key).disabled = box.checked;
  }));

  $("alertForm").addEventListener("submit", async ev => {
    ev.preventDefault();
    const l = link;
    if (!l) return;
    const plan = [], removed = [];
    for (const dest of Object.keys(DESTS)) {
      if (removeBox(dest).checked) {
        removed.push(DESTS[dest]);
        for (const f of FIELDS) if (f.dest === dest) plan.push({ f, value: "" });
        continue;
      }
      for (const f of FIELDS) {
        if (f.dest !== dest || !f.dirty) continue;
        const v = valueOf(f);
        if (v === "") continue;                          // empty keeps what is on the board
        const bad = badText(v, f.label, f.max) || (f.re && !f.re.test(v) ? f.form : "");
        if (bad) return invalid($(f.key), "alertMsg", bad);
        plan.push({ f, value: v });
      }
    }
    if (!plan.length) return say("alertMsg", "Nothing to save. Type a value, or select a box to remove a destination.", "bad");
    const btn = $("saveAlerts");
    busy(btn, true);
    say("alertMsg", "The page sends the alert settings to the board.", "busy");
    const saved = [];
    try {
      for (const { f, value } of plan) {
        if (f.secret) l.secret(value);
        const r = await l.request("alert set " + f.key + ' "' + value + '"', want.done("alert"), 5000,
          "alert set " + f.key + ' "' + (f.secret && value ? "[hidden]" : value) + '"');
        if (!r.ok) {
          say("alertMsg", "The board did not save the " + f.label + ": " + r.error + ".", "bad");
          return;
        }
        if (value) saved.push(f.label);
      }
      for (const { f } of plan) { $(f.key).value = ""; f.dirty = false; }
      for (const dest of Object.keys(DESTS)) {
        const box = removeBox(dest);
        if (box.checked) { box.checked = false; box.dispatchEvent(new Event("change")); }
      }
      say("alertMsg", [saved.length ? "Saved on the board: " + saved.join(", ") + "." : "",
        removed.length ? "Removed from the board: " + removed.join(", ") + "." : "",
        "The board uses the new settings at once. Click Send test to try a destination."].filter(Boolean).join(" "), "ok");
    } catch (e) {
      say("alertMsg", e.code === "timeout" ? "The board did not answer." : "The command did not go to the board: " + e.message, "bad");
    } finally {
      busy(btn, false);
    }
  });

  const TEST_HINT = {
    "not connected to Wi-Fi": " Connect the board to Wi-Fi first.",
    "alerts are not running": " Alerts start when Wi-Fi is on. Save Wi-Fi first.",
    "token or chat id not set": " Save the bot token and the chat ID first.",
    "address not set": " Save the webhook address first.",
    "broker not set": " Save the broker address first.",
    "not connected to the broker": " Make sure that the broker address, the user and the password are correct.",
  };

  document.querySelectorAll("[data-test]").forEach(btn => btn.addEventListener("click", async () => {
    const l = link;
    if (!l) return;
    const dest = btn.dataset.test;
    const out = document.querySelector('[data-result="' + dest + '"]');
    const unsaved = removeBox(dest).checked || FIELDS.some(f => f.dest === dest && f.dirty && valueOf(f) !== "");
    busy(btn, true);
    say(out, "The board sends a test. This can take up to 30 seconds.", "busy");
    try {
      const r = await l.request("alert test " + dest, want.test, 40000);
      const detail = typeof r.detail === "string" ? r.detail.trim() : "";
      let text = r.ok
        ? "Sent." + (r.status ? " The server answered " + r.status + "." : "") + " Time: " + r.ms + " ms."
        : "Not sent: " + (detail || "no reason given") + (r.status ? " (answer " + r.status + ")" : "") + "." + (TEST_HINT[detail] || "");
      if (unsaved) text += " The test used the settings on the board. Your changes in this box are not saved.";
      say(out, text, r.ok ? "ok" : "bad");
    } catch (e) {
      say(out, e.code === "timeout" ? "The board did not answer in 40 seconds." : "The command did not go to the board: " + e.message, "bad");
    } finally {
      busy(btn, false);
    }
  }));

  // ---- restart ----
  async function findPortAgain(old, ms) {
    const end = Date.now() + ms;
    while (Date.now() < end) {
      await sleep(1000);
      let ports = [];
      try { ports = await serial.getPorts(); } catch (e) { ports = []; }
      const same = ports.filter(p => {
        const i = p.getInfo ? p.getInfo() : {};
        return i.usbVendorId === old.usbVendorId && i.usbProductId === old.usbProductId && !p.readable;
      });
      if (same.length) return same[0];
    }
    return null;
  }

  $("restart").addEventListener("click", async () => {
    const l = link;
    if (!l) return;
    const btn = $("restart");
    restarting = true;
    busy(btn, true);
    forms.disabled = true;
    setState(false, "Restarting");
    say("restartMsg", "The board restarts. The page waits for it to answer again.", "busy");
    const old = l.port.getInfo ? l.port.getInfo() : {};
    try {
      const boot = l.waitFor(want.info, 25000);         // main.c prints {"info":..} at the end of its start
      await l.send("reboot");
      const how = await boot.then(() => "boot", e => (e.code === "lost" ? "lost" : "quiet"));
      if (how === "lost") {
        // the port went away with the restart (a board that re-enumerates on USB): open it again when it is back
        link = null;
        say("restartMsg", "The board restarts. The page waits for its USB port to come back.", "busy");
        const port = await findPortAgain(old, 20000);
        const again = port ? await openLink(port) : null;
        if (!again) {
          setConnected(false);
          say("restartMsg", "The board restarted, but the page cannot open its port again. Click Connect to the board.", "bad");
          return;
        }
      } else {
        info = await waitReady(l, 25000);
        renderSummary();
        await refreshWifi(l);
        ready();
      }
      say("restartMsg", "The board restarted and answers.", "ok");
    } catch (e) {
      if (link === l) {
        await l.close();
        link = null;
      }
      setConnected(false);
      say("restartMsg", "The board did not answer after the restart. Disconnect the USB cable, connect it again, and click Connect to the board.", "bad");
    } finally {
      restarting = false;
      busy(btn, false);
      if (link) ready();
    }
  });

  if (demo) logAdd("Demo mode: the page uses a simulated board.", "sys");
}
