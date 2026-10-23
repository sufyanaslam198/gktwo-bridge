// Runs the page script (argv[2] = extracted JS) against a tiny fake DOM, a fake WebSocket and a scripted fetch.
// Prints one JSON object of observations; driven by test_page_js.py.
const fs = require("fs");
const src = fs.readFileSync(process.argv[2], "utf8");
const scenario = process.argv[3];

const els = {};
function mk(id) {
  return { id, style: {}, _t: "", value: 0, disabled: false, className: "", children: [], title: "",
           get textContent() { return this._t; },
           set textContent(v) { this._t = v; if (v === "") this.children = []; }, // like the DOM
           appendChild(c) { this.children.push(c); return c; }, onclick: null, onchange: null };
}
const document = {
  getElementById: (id) => els[id] || (els[id] = mk(id)),
  createElement: () => mk(null),
};
const MB = 1048576;
const MID = "0000aabbccddeeff";
const socks = [];
class FakeWS {
  constructor(url) { this.url = url; this.sent = []; this.readyState = 0; socks.push(this); }
  send(s) { this.sent.push(JSON.parse(s)); }
  close() { this.readyState = 3; }
  open() { this.readyState = 1; if (this.onopen) this.onopen({}); }
  recv(o) { if (this.onmessage) this.onmessage({ data: JSON.stringify(o) }); }
  drop() { this.readyState = 3; if (this.onclose) this.onclose({}); }
}
FakeWS.OPEN = 1;
const cmds = (ws) => ws.sent.map((m) => m.Data.Cmd);
const last = (ws, cmd) => ws.sent.filter((m) => m.Data.Cmd === cmd).pop();

const confirms = [];
const fetches = [];
let fetchPlan = () => ({ status: 200, body: { code: "000000", messages: null, data: {}, success: true } });
function fetchStub(url, opts) {
  const f = { url, method: opts && opts.method, fields: {}, file: null };
  if (opts && opts.body) for (const [k, v, n] of opts.body.entries) { if (k === "File") f.file = { size: v.size, name: n }; else f.fields[k] = v; }
  fetches.push(f);
  const r = fetchPlan(fetches.length - 1);
  if (!r) return new Promise(() => {});        // never answers
  return Promise.resolve({ ok: r.status < 300, status: r.status, text: () => Promise.resolve(JSON.stringify(r.body)) });
}
class FormDataStub { constructor() { this.entries = []; } append(k, v, n) { this.entries.push([k, v, n]); } }
const timers = [], intervals = [];
let uuidN = 0;
const g = {
  document, fetch: fetchStub, WebSocket: FakeWS, FormData: FormDataStub, console,
  confirm: (t) => { confirms.push(t); return true; },
  location: { hostname: "printer.local", protocol: "http:" },
  crypto: { randomUUID: () => "11111111-2222-4333-8444-" + String(++uuidN).padStart(12, "0") },
  setTimeout: (f, ms) => { timers.push({ f, ms, live: true }); return timers.length; },
  clearTimeout: (id) => { if (timers[id - 1]) timers[id - 1].live = false; },
  setInterval: (f, ms) => { intervals.push({ f, ms }); return intervals.length; },
  clearInterval: () => {},
  AbortController: class { constructor() { this.signal = {}; } abort() {} },
};
g.window = g;
new Function(...Object.keys(g), src)(...Object.values(g));
const tick = () => new Promise((r) => setImmediate(r));
async function settle() { for (let i = 0; i < 20; i++) await tick(); }
const fire = (ms) => timers.filter((t) => t.ms === ms && t.live).forEach((t) => { t.live = false; t.f(); });
const btns = () => els["file-list"].children.flatMap((tr) => tr.children[1].children);

const frame = (kind, key, body, extra) => Object.assign({ Id: "x", [key]: body, MainboardID: MID, TimeStamp: 1, Topic: "sdcp/" + kind + "/" + MID }, extra || {});
function status(cur, sub, o) {
  o = o || {};
  const z = o.z === undefined ? { Pos: 12.35, Homed: true, Moving: false } : o.z;
  return frame("status", "Status", { CurrentStatus: [cur], PreviousStatus: 0,
    PrintInfo: { Status: sub, CurrentLayer: o.layer || 0, TotalLayer: o.total || 0, CurrentTicks: o.cur || 0, TotalTicks: o.tot || 0,
                 Filename: o.file || "", ErrorNumber: 0, TaskId: "" },
    GKBridge: { HookOk: o.hook !== false, Z: z, Message: o.message || "" } });
}
const attrs = (usb, free) => frame("attributes", "Attributes", { UsbDiskStatus: usb, RemainingMemory: free, MainboardID: MID });
const respond = (req, ack, extra) => ({ Id: "x", Topic: "sdcp/response/" + MID,
  Data: { Cmd: req.Data.Cmd, Data: Object.assign({ Ack: ack }, extra || {}), RequestID: req.Data.RequestID, MainboardID: MID, TimeStamp: 1 } });
const notice = (m, type) => ({ Id: "x", Data: { Data: { Message: m, Type: type === undefined ? 100 : type }, MainboardID: MID, TimeStamp: 1 }, Topic: "sdcp/notice/" + MID });
const listing = (names) => names.map((n) => ({ name: n, usedSize: 0, totalSize: 1, storageType: 0, type: /\.\w+$/.test(n) ? 1 : 0 }));

// Opens the socket and answers the file list request.
async function connect(opts) {
  opts = opts || {};
  const ws = socks[0];
  ws.open();
  await settle();
  const req = last(ws, 258);
  if (req) ws.recv(respond(req, 0, { FileList: listing(opts.files || ["/local/a.ctb"]) }));
  await settle();
  return ws;
}

(async () => {
  const out = {};
  if (scenario === "connect") {
    const ws = socks[0];
    out.url = ws.url;
    ws.open();
    await settle();
    out.cmds = cmds(ws);
    out.keepalive_ms = intervals.map((i) => i.ms);
    const n = ws.sent.length;
    intervals.forEach((i) => i.f());
    out.keepalive_cmds = cmds(ws).slice(n);
    out.rids = new Set(ws.sent.map((m) => m.Data.RequestID)).size === ws.sent.length;
    out.topic = ws.sent[0].Topic;
  } else if (scenario === "status_printing") {
    const ws = await connect();
    ws.recv(status(1, 3, { layer: 5, total: 10, cur: 60000, tot: 180000, file: "/local/a.ctb" }));
    await settle();
    out.state = els["status-state"].textContent;
    out.layer = els["status-layer"].textContent;
    out.elapsed = els["status-elapsed"].textContent;
    out.remaining = els["status-remaining"].textContent;
    out.progress = els["status-progress"].value;
    out.file = els["status-file"].textContent;
    out.ctl = ["btn-pause", "btn-resume", "btn-stop"].map((i) => els[i].disabled);
    out.print_del = btns().map((b) => b.disabled);
    out.zpos = els["z-pos"].textContent;
    ws.recv(status(1, 6, { file: "/local/a.ctb", message: "careful <b>x</b>" }));
    out.paused = els["status-state"].textContent;
    out.paused_ctl = ["btn-pause", "btn-resume", "btn-stop"].map((i) => els[i].disabled);
    out.msg = els["status-message"].textContent;
  } else if (scenario === "notice_after_nack" || scenario === "nack_without_notice" || scenario === "ack_ok") {
    const ws = await connect();
    ws.recv(status(1, 3));
    await settle();
    els["btn-pause"].onclick();
    const req = last(ws, 129);
    out.cmd = req.Data.Cmd;
    out.disabled_during = els["btn-pause"].disabled;
    ws.recv(respond(req, scenario === "ack_ok" ? 0 : 1));
    if (scenario === "notice_after_nack") ws.recv(notice("printer busy"));
    out.before_timer = document.getElementById("message").textContent;
    fire(1000);
    await settle();
    out.msg = document.getElementById("message").textContent;
    out.cls = document.getElementById("message").className;
  } else if (scenario === "start_fails_later" || scenario === "start_then_prints" || scenario === "start_cancelled_elsewhere") {
    const ws = await connect();
    ws.recv(status(0, 0));
    await settle();
    btns()[0].onclick();
    ws.recv(respond(last(ws, 128), 0));
    ws.recv(status(1, 10));
    await settle();
    out.sent = document.getElementById("message").textContent;
    if (scenario === "start_then_prints") ws.recv(status(1, 3));
    if (scenario === "start_cancelled_elsewhere") ws.recv(status(0, 8));   // another client cancelled: no notice for us
    ws.recv(notice("USB drive removed during copy"));    // after Ack 0: the copy or the start failed
    fire(1000);
    out.msg = document.getElementById("message").textContent;
    out.cls = document.getElementById("message").className;
  } else if (scenario === "notice_other_type") {
    const ws = await connect();
    ws.recv(status(1, 3));
    await settle();
    els["btn-pause"].onclick();
    ws.recv(respond(last(ws, 129), 1));
    ws.recv(notice("something else", 0));         // not a refusal text
    out.before_timer = document.getElementById("message").textContent;
    fire(1000);
    out.msg = document.getElementById("message").textContent;
  } else if (scenario === "hook_down") {
    const ws = await connect();
    ws.recv(status(0, 0, { hook: false, message: "hook: not running", z: null }));
    await settle();
    out.buttons = btns().map((b) => b.disabled);
    out.z = els["z-pos"].textContent;
    out.msg = els["status-message"].textContent;
    ws.recv(status(0, 0));
    out.buttons_ok = btns().map((b) => b.disabled);
  } else if (scenario === "upload_empty") {
    const ws = await connect();
    ws.recv(status(0, 0));
    await settle();
    const inp = els["upload-input"];
    inp.files = [{ name: "e.ctb", size: 0, slice: (a, b) => ({ size: b - a }) }];
    inp.onchange.call(inp);
    await settle();
    out.fetches = fetches.length;
    out.msg = document.getElementById("message").textContent;
    out.cls = document.getElementById("message").className;
  } else if (scenario === "print_nonctb") {
    const ws = await connect({ files: ["/local/a.ctb", "/local/b.cbddlp"] });
    ws.recv(status(0, 0));
    await settle();
    out.buttons = btns().map((b) => b.disabled);
    out.titles = btns().map((b) => b.title || "");
    btns()[0].onclick();
    out.start = last(ws, 128).Data.Data;
    btns()[1].onclick();
    out.delete = last(ws, 259) ? last(ws, 259).Data.Data : null;
  } else if (scenario === "usb_tab") {
    const ws = await connect();
    out.usb_hidden = els["tab-usb"].style.display;
    ws.recv(attrs(1, 100 * MB));
    await settle();
    out.usb_shown = els["tab-usb"].style.display;
    out.free = els["free-space"].textContent;
    els["tab-usb"].onclick();
    out.list_url = last(ws, 258).Data.Data.Url;
    ws.recv(respond(last(ws, 258), 0, { FileList: listing(["/usb/u.ctb"]) }));
    ws.recv(status(0, 0));
    await settle();
    out.rows = els["file-list"].children.length;
    btns()[0].onclick();
    out.start = last(ws, 128).Data.Data;
    ws.recv(attrs(0, 100 * MB));
    await settle();
    out.usb_gone = els["tab-usb"].style.display;
    out.list_url_after = last(ws, 258).Data.Data.Url;
  } else if (scenario.startsWith("upload_")) {
    const ws = await connect();
    ws.recv(attrs(0, 500 * MB));
    ws.recv(status(0, 0));
    await settle();
    if (scenario === "upload_fail") fetchPlan = () => ({ status: 409, body: { code: "111111", messages: [{ field: "common_field", message: -4 }, { field: "common_field", message: "printer busy" }], data: null, success: false } });
    if (scenario === "upload_fail_code") fetchPlan = () => ({ status: 500, body: { code: "111111", messages: [{ field: "common_field", message: -2 }], data: null, success: false } });
    if (scenario === "upload_cancel") fetchPlan = () => null;
    const inp = els["upload-input"];
    inp.files = [{ name: "big.ctb", size: 2 * MB + MB / 2, slice: (a, b) => ({ size: b - a }) }];
    inp.onchange.call(inp);
    await settle();
    if (scenario === "upload_cancel") {
      out.cancel_enabled = !els["btn-upload-cancel"].disabled;
      els["btn-upload-cancel"].onclick();
      await settle();
      out.cancel = last(ws, 255) ? last(ws, 255).Data.Data : null;
    }
    out.fetches = fetches.map((f) => ({ url: f.url, method: f.method, offset: f.fields.Offset, total: f.fields.TotalSize,
                                        uuid: f.fields.Uuid, check: f.fields.Check, size: f.file && f.file.size, name: f.file && f.file.name }));
    out.msg = document.getElementById("message").textContent;
    out.cls = document.getElementById("message").className;
    out.progress = els["upload-progress"].value;
  } else if (scenario === "z_buttons") {
    const ws = await connect();
    ws.recv(status(0, 0));
    await settle();
    out.enabled = ["btn-z-up10", "btn-z-top", "btn-z-home"].map((i) => els[i].disabled);
    for (const i of ["btn-z-up10", "btn-z-dn1", "btn-z-top", "btn-z-home", "btn-z-stop"]) {
      els[i].onclick();
      await settle();
      ws.recv(respond(ws.sent[ws.sent.length - 1], 0));
      await settle();
    }
    out.sent = ws.sent.filter((m) => m.Data.Cmd >= 4096).map((m) => [m.Data.Cmd, m.Data.Data]);
    out.confirms = confirms.length;
    ws.recv(status(0, 0, { z: { Pos: null, Homed: false, Moving: false } }));
    await settle();
    out.unhomed = ["btn-z-up10", "btn-z-top", "btn-z-home"].map((i) => els[i].disabled);
    out.hint = els["z-hint"].textContent;
    ws.recv(status(1, 3));
    await settle();
    out.busy = ["btn-z-up10", "btn-z-top", "btn-z-home", "btn-z-stop"].map((i) => els[i].disabled);
  } else if (scenario === "reconnect_no_flicker") {
    const ws = await connect();
    ws.recv(status(0, 0));
    await settle();
    ws.drop();
    out.after_first_drop = els["banner"].style.display;
    out.state_cleared = els["status-state"].textContent;
    out.reconnect_timer = timers.some((t) => t.ms === 3000 && t.live);
    fire(3000);
    out.sockets = socks.length;
    socks[1].drop();                           // the reconnect attempt fails
    out.after_second_drop = els["banner"].style.display;
    fire(3000);
    socks[2].open();
    await settle();
    out.after_open = els["banner"].style.display;
  }
  console.log(JSON.stringify(out));
})();
