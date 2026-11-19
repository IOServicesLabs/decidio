// decidio installer, steps 1 to 3: the board, the idea and the ESP Web Tools button. Step 4 is in setup.js.
// Reads builds/index.json (written by tools/make_builds.py). Remembers only the last board and idea in localStorage.
import { initSetup } from "./setup.js";

const $ = id => document.getElementById(id);
const el = (tag, props, ...kids) => { const e = Object.assign(document.createElement(tag), props || {}); e.append(...kids); return e; };

const params = new URLSearchParams(location.search);
const demo = params.has("demo");
const mobile = navigator.userAgentData ? navigator.userAgentData.mobile : /Android|iPhone|iPad|iPod|Mobile/i.test(navigator.userAgent);
const hasSerial = "serial" in navigator;
const secure = window.isSecureContext;

const store = {
  get(k) { try { return localStorage.getItem("decidio." + k); } catch (e) { return null; } },
  set(k, v) { try { v == null ? localStorage.removeItem("decidio." + k) : localStorage.setItem("decidio." + k, v); } catch (e) { /* private window */ } },
};

let index = null, board = null, picked = null;

// ---- browser notices -------------------------------------------------------------------------------------------------
function notices() {
  const n = $("browserNotice");
  let text = "";
  if (mobile) {
    text = "A phone cannot install or set up a board from this page. Use Chrome or Edge on a computer. You can read the boards and the ideas here.";
  } else if (!hasSerial) {
    text = "This browser cannot connect to USB devices. To install or to set up a board, use Chrome or Edge on a computer. You can read the boards and the ideas here.";
  } else if (!secure) {
    text = "Open this page over https:// or from localhost. The browser does not allow USB from this address.";
  }
  if (text) {
    n.replaceChildren(el("b", { textContent: "USB is not available. " }), text);
    n.hidden = false;
  }
  if (demo) {
    $("demoBanner").hidden = false;
    const u = new URL(location.href);
    u.searchParams.delete("demo");
    $("demoExit").href = u.pathname + (u.search || "") + u.hash;
  }
}

// ---- step 1 ----------------------------------------------------------------------------------------------------------
function drawBoards() {
  const ids = Object.keys(index.boards);
  $("boards").replaceChildren(...ids.map(id => {
    const input = el("input", { type: "radio", name: "board", value: id, checked: id === board });
    input.addEventListener("change", () => { if (input.checked) setBoard(id); });
    return el("label", { className: "tile" }, input,
      el("span", null, el("b", { textContent: index.boards[id].name }), el("small", { textContent: id })));
  }));
}

function setBoard(id) {
  board = id;
  store.set("board", id);
  if (picked && !picked.boards[board]) {
    pick(null);
  } else {
    pick(picked);
  }
  drawIdeas();
}

// ---- step 2 ----------------------------------------------------------------------------------------------------------
function tags(e) {
  const t = [el("span", { className: "tag num", textContent: "#" + e.idea })];
  if (e.mic) t.push(el("span", { className: "tag", textContent: "microphone" }));
  if (e.radio) t.push(el("span", { className: "tag", textContent: "Wi-Fi signal" }));
  if (e.photo) t.push(el("span", { className: "tag", textContent: "sends photos" }));
  return el("span", { className: "tags" }, ...t);
}

function drawIdeas() {
  const f = $("filter").value.trim().toLowerCase();
  const all = index.ideas.filter(e => e.boards[board]);
  const words = f.split(/\s+/).filter(Boolean);
  const list = all.filter(e => {
    const text = (e.title + " " + e.setup + " " + e.needs.join(" ") + " " + e.id + " #" + e.idea).toLowerCase();
    return words.every(w => text.includes(w));
  });
  const rows = list.map(e => {
    const input = el("input", { type: "radio", name: "idea", value: e.id, checked: !!picked && picked.id === e.id });
    input.addEventListener("change", () => { if (input.checked) pick(e); });
    return el("label", { className: "idea" }, input,
      el("span", null, el("span", { className: "t" }, e.title, " ", tags(e)), el("span", { className: "s", textContent: e.setup })));
  });
  $("list").replaceChildren(...(rows.length ? rows : [el("p", { className: "empty", textContent: "No idea agrees with your search. Type a different word." })]));
  const hidden = index.ideas.length - all.length;
  $("count").textContent = "The list shows " + list.length + " of " + all.length + " ideas for this board." +
    (hidden ? " " + hidden + " ideas need a camera or a microphone that this type of board does not have. They are not shown." : "") + " Firmware " + index.version + ".";
}

// ---- step 3 ----------------------------------------------------------------------------------------------------------
function pick(e) {
  picked = e && e.boards[board] ? e : null;
  store.set("idea", picked ? picked.id : null);
  const btn = $("install");
  if (picked) {
    $("chosenTitle").textContent = picked.title;
    $("chosenSetup").textContent = "Setup: " + picked.setup;
    btn.setAttribute("manifest", "builds/" + picked.boards[board].manifest);
    btn.hidden = false;
    $("pickedTitle").textContent = picked.title;
    $("picked").hidden = false;
  } else {
    $("chosenTitle").textContent = "No idea is selected.";
    $("chosenSetup").textContent = "Select an idea in step 2.";
    btn.removeAttribute("manifest");
    btn.hidden = true;
    $("picked").hidden = true;
  }
}

// ---- footer flag -------------------------------------------------------------------------------------------------------
function usFlag() {
  let s = '<svg viewBox="0 0 760 400" role="img" aria-label="Flag of the United States"><rect width="760" height="400" fill="var(--card)"/><g fill="var(--ink)">';
  for (let k = 0; k < 7; k++) s += '<rect y="' + (k * 61.54).toFixed(2) + '" width="760" height="30.77"/>';
  s += '<rect width="304" height="215.38"/></g><g fill="var(--card)">';
  for (let r = 1; r <= 9; r++) for (let c = 1; c <= (r % 2 ? 6 : 5); c++) s += '<circle r="7" cx="' + ((r % 2 ? c * 2 - 1 : c * 2) * 25.33).toFixed(1) + '" cy="' + (r * 21.54).toFixed(1) + '"/>';
  return s + "</g></svg>";
}

// ---- start -------------------------------------------------------------------------------------------------------------
$("flag").innerHTML = usFlag();
notices();
initSetup({ demo, mobile, boardName: id => (index && index.boards[id] ? index.boards[id].name : null), version: () => (index ? index.version : null) });

fetch("builds/index.json", { cache: "no-cache" }).then(r => {
  if (!r.ok) throw new Error(r.status);
  return r.json();
}).then(j => {
  index = j;
  const saved = store.get("board");
  board = j.boards[saved] ? saved : Object.keys(j.boards)[0];
  drawBoards();
  const want = store.get("idea");
  pick(j.ideas.find(e => e.id === want) || null);
  drawIdeas();
  $("filter").addEventListener("input", drawIdeas);
}).catch(() => {
  const msg = "No builds were found next to this page. Make them with: python tools/make_builds.py";
  $("boards").replaceChildren(el("p", { className: "msg bad", textContent: msg }));
  $("list").replaceChildren(el("p", { className: "empty", textContent: msg }));
});
