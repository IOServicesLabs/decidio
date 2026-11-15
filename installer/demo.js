// Demo mode (?demo in the address): a simulated board on the serial side, so that step 4 can be tried without hardware.
// It answers the console commands the way firmware/main/console.c, wifi.c, alert.c and store.c do: the same JSON
// lines, the same order ({"error"} before {"ok"}), the same checks. Nothing leaves the page.
//   Wi-Fi password "wrongpassword"  -> the board stays at "connecting", reason 15 (wrong password)
//   anything else                   -> online after about 3 s
const enc = new TextEncoder();
const sleep = ms => new Promise(r => setTimeout(r, ms));
const bytes = s => enc.encode(s).length;

export function createDemoSerial() {
  const port = new DemoPort();
  return {
    async requestPort() { await sleep(250); return port; },
    async getPorts() { return [port]; },
    addEventListener() {},
    removeEventListener() {},
  };
}

class DemoPort {
  constructor() {
    this.readable = null;
    this.writable = null;
    this.ctrl = null;
    this.board = new DemoBoard(text => this.emit(text));
  }
  getInfo() { return { usbVendorId: 0x303a, usbProductId: 0x1001 }; }     // as the XIAO's USB-Serial-JTAG
  async open() {
    if (this.readable) throw new DOMException("The port is already open.", "InvalidStateError");
    const dec = new TextDecoder();
    this.readable = new ReadableStream({ start: c => { this.ctrl = c; }, cancel: () => { this.ctrl = null; } });
    this.writable = new WritableStream({ write: chunk => this.board.input(dec.decode(chunk, { stream: true })) });
    this.board.attach();
  }
  async setSignals() {}
  async close() {
    this.board.detach();
    try { if (this.ctrl) this.ctrl.close(); } catch (e) { /* cancelled */ }
    this.ctrl = null;
    this.readable = null;
    this.writable = null;
  }
  emit(text) {
    if (!this.ctrl) return;
    try { this.ctrl.enqueue(enc.encode(text)); } catch (e) { /* closed */ }
  }
}

// words, or "a few words" in double quotes (console.c next_arg)
function args(line) {
  const out = [];
  let i = 0;
  for (;;) {
    while (i < line.length && " \t".includes(line[i])) i++;
    if (i >= line.length) return out;
    if (line[i] === '"') {
      const j = line.indexOf('"', i + 1), end = j < 0 ? line.length : j;
      out.push(line.slice(i + 1, end));
      i = end + 1;
    } else {
      let j = i;
      while (j < line.length && !" \t".includes(line[j])) j++;
      out.push(line.slice(i, j));
      i = j;
    }
  }
}

const QUESTIONS = ["person", "animal", "vehicle", "package", "motion", "light", "door", "knock", "speech", "baby_cry", "alarm_sound", "glass_break"];

class DemoBoard {
  constructor(out) {
    this.out = out;
    this.host = "decidio-3f2a";
    this.store = new Map();
    this.buf = "";
    this.queue = Promise.resolve();
    this.ready = true;
    this.attached = false;
    this.tick = 0;
    this.wifi = { state: "setup", setup_open: true, ip: "", rssi: 0, channel: 0, reason: 0 };
  }

  attach() {
    this.attached = true;
    if (!this.timer) this.timer = setInterval(() => this.periodic(), 1000);
  }
  detach() {
    this.attached = false;
    clearInterval(this.timer);
    this.timer = null;
  }
  line(x) { this.out((typeof x === "string" ? x : JSON.stringify(x)) + "\r\n"); }

  input(text) {
    this.buf += text;
    let i;
    while ((i = this.buf.search(/[\r\n]/)) >= 0) {
      let l = this.buf.slice(0, i);
      this.buf = this.buf.slice(i + 1);
      if (!l) continue;
      while (bytes(l) > 255) l = l.slice(0, -1);       // console.c keeps 255 bytes of a line
      this.queue = this.queue.then(() => this.run(l)).catch(() => {});
    }
  }

  async run(raw) {
    while (!this.ready) await sleep(50);               // typed lines wait in the driver's buffer during start-up
    await sleep(30 + Math.random() * 70);
    const a = args(raw), cmd = a[0], arg = a[1];
    if (!cmd) return;
    if (cmd === "info") {
      this.line(this.info());
    } else if (cmd === "wifi") {
      this.wifiCommand(arg, a[2]);
    } else if (cmd === "alert" && arg === "test") {
      await this.test(a[2] || "probe");
    } else if (cmd === "alert" && arg === "set") {
      const key = a[2], val = a[3];
      if (!key || key.length > 15 || (val !== undefined && /["\\]/.test(val))) {
        this.line({ error: "alert set <key> <value>" });
        return;
      }
      if (val) this.store.set(key, val); else this.store.delete(key);        // store.c: NULL or "" removes the key
    } else if (cmd === "reboot") {
      await this.reboot();
      return;
    } else if (["run", "stop", "print", "mem", "board", "inputs", "eye", "orient", "profile", "collect"].includes(cmd)) {
      // accepted, not simulated
    } else {
      this.line({ error: "unknown command " + cmd });
      return;
    }
    this.line({ ok: cmd });
  }

  wifiCommand(arg, arg2) {
    if (arg === undefined) {
      const w = this.wifi;
      this.line({ wifi: { state: w.state, setup_open: w.setup_open, ip: w.ip, host: this.host, rssi: w.rssi, channel: w.channel, reason: w.reason } });
    } else if ((arg === "forget" || arg === "off") && arg2 === undefined) {
      this.store.delete("wifi_ssid");
      this.store.delete("wifi_pass");
      clearTimeout(this.joinT);
      this.wifi = arg === "off" ? { state: "off", setup_open: false, ip: "", rssi: 0, channel: 0, reason: 0 }
        : { state: "setup", setup_open: true, ip: "", rssi: 0, channel: 0, reason: 0 };
    } else if (!arg || bytes(arg) > 32 || (arg2 && bytes(arg2) > 64)) {
      this.line({ error: "wifi: ESP_ERR_INVALID_ARG" });          // wifi.c rt_wifi_set; console.c still prints ok after it
    } else {
      this.store.set("wifi_ssid", arg);
      this.store.set("wifi_pass", arg2 || "");
      this.join();
    }
  }

  join(delay = 3200) {
    clearTimeout(this.joinT);
    this.wifi = Object.assign({}, this.wifi, { state: "connecting", ip: "", rssi: 0, channel: 0, reason: 0 });
    this.joinT = setTimeout(() => {
      if (this.store.get("wifi_pass") === "wrongpassword") {
        this.wifi.reason = 15;
        this.line("W (" + (Date.now() % 100000) + ") wifi: 4-way handshake timeout");
        return;
      }
      this.wifi = Object.assign({}, this.wifi, { state: "online", ip: "192.168.1.57", rssi: -58, channel: 6, reason: 0 });
      this.line({ wifi: { state: "online", ip: this.wifi.ip, open: "http://" + this.host + ".local" } });
    }, delay);
  }

  async test(dest) {
    await sleep(dest === "mqtt" ? 1400 : 900);
    const r = { ok: false, status: 0, ms: 0, detail: "" };
    const has = k => this.store.has(k);
    if (this.wifi.state === "off") {
      r.detail = "alerts are not running";
    } else if (this.wifi.state !== "online") {
      r.detail = "not connected to Wi-Fi";
    } else if (dest === "probe") {
      Object.assign(r, { ok: true, status: 404, ms: 640 });
    } else if (dest === "telegram") {
      if (has("tg_token") && has("tg_chat")) Object.assign(r, { ok: true, status: 200, ms: 812 });
      else r.detail = "token or chat id not set";
    } else if (dest === "webhook") {
      if (has("hook_url")) Object.assign(r, { ok: true, status: 200, ms: 433 });
      else r.detail = "address not set";
    } else if (dest === "mqtt") {
      if (has("mqtt_url")) Object.assign(r, { ok: true, status: 0, ms: 118 });
      else r.detail = "broker not set";
    }
    this.line('{"alert_test":{"ok":' + r.ok + ',"status":' + r.status + ',"ms":' + r.ms + ',"detail":"' + r.detail +
      '"},"heap_internal":61234,"heap_min":40211}');
  }

  async reboot() {
    this.ready = false;
    clearTimeout(this.joinT);
    await sleep(200);
    this.out("\u0000\u00ff\u0013ESP-ROM:esp32s3-20210327\r\n");
    this.line("Build:Mar 27 2021");
    this.line("rst:0xc (RTC_SW_CPU_RST),boot:0x8 (SPI_FAST_FLASH_BOOT)");
    await sleep(700);
    this.line("I (612) board: xiao_sense: camera yes, microphone yes (spread 14)");
    this.line("I (1180) model: model 'general': " + QUESTIONS.length + " questions, image yes, audio yes, 391204 B");
    await sleep(900);
    this.line(this.info());                           // main.c: rt_print_info(), then rt_console_ready()
    this.ready = true;
    if (this.store.get("wifi_ssid")) {
      this.wifi = { state: "connecting", setup_open: false, ip: "", rssi: 0, channel: 0, reason: 0 };
      this.join(2200);
    } else {
      this.wifi = { state: "setup", setup_open: true, ip: "", rssi: 0, channel: 0, reason: 0 };
      this.line({ wifi: { state: "setup", join: this.host, open: "http://192.168.4.1" } });
    }
  }

  periodic() {
    if (!this.attached || !this.ready) return;
    this.tick++;
    const p = () => Math.round(Math.random() * 60) / 1000;
    const answers = {};
    for (const q of QUESTIONS) answers[q] = q === "light" ? { choice: "day", confidence: 0.912, probabilities: { day: 0.912, dim: 0.07, dark: 0.018 } } : { p_true: p() };
    this.line({ answers, hz: 9.1 });
    if (this.tick % 20 === 0) this.line({ event: "person-seen", state: this.tick % 40 ? "on" : "off", p: 0.912, ms: this.tick * 1000 });
  }

  info() {
    const options = {};
    for (const q of QUESTIONS) options[q] = q === "light" ? ["day", "dim", "dark"] : ["no", "yes"];
    return {
      info: {
        fw: "decidio-rt 0.4.0", board: "xiao_sense", camera: "OV3660", mic: true,
        sensors: { profile: "xiao_sense", saved: true, camera: "OV3660", mic: true, mic_spread: 14, card: false, radio: true, psram_mb: 8, flash_mb: 8 },
        model: { name: "general", image: { size: 96, colors: 1, motion: 1 }, audio: { sr: 16000, clip: 16000 }, questions: QUESTIONS,
          options, na: [], arena_used: 391204 },
        pins: [1, 2, 3, 4, 5, 6, 7, 8, 9, 21, 43, 44], collect: { colors: 1, audio: true }, heap_internal: 61234, psram: 6123456,
      },
    };
  }
}
