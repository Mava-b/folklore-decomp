// Controls page: the bindings in input.toml (PS3_INPUT_MAP, read by the
// game's cellPad; format in the toolkit's libs/input/pad_input_map.h, the
// same file the inFAMOUS 2 and Resistance ports' launchers write). Click a cell, then press
// the key, mouse button or controller button to bind it; the rows light up
// while their inputs are held, and the sticks below follow the controller,
// which together are the controller test. Until a binding changes there is
// no file and the game uses its built-in layout (the one in DEFAULTS).
import { invoke } from "@tauri-apps/api/core";
import { Key, t } from "./i18n";
import { flashSaved } from "./ui";

export type Device = "keyboard" | "mouseButtons" | "controller";
export type Bindings = Record<Device, Record<string, string[]>>;
interface InputMapView {
  path: string;
  exists: boolean;
  bindings: Bindings;
}

const DEVICES: Device[] = ["keyboard", "mouseButtons", "controller"];

/** PS3 controls, in the screen's order, with the label shown for each. What
 * each does in Folklore is not documented in the repository (README,
 * folklore/docs), so no action is named (`act` stays for the shared code). */
const CONTROLS: { id: string; label: string | Key; act?: Key }[] = [
  { id: "r1", label: "R1" },
  { id: "l1", label: "L1" },
  { id: "l2", label: "L2" },
  { id: "r2", label: "R2" },
  { id: "cross", label: "✕" },
  { id: "circle", label: "○" },
  { id: "square", label: "□" },
  { id: "triangle", label: "△" },
  { id: "l3", label: "L3" },
  { id: "r3", label: "R3" },
  { id: "start", label: "START" },
  { id: "select", label: "SELECT" },
  { id: "up", label: "controls.dpad" },
  { id: "down", label: "controls.dpad" },
  { id: "left", label: "controls.dpad" },
  { id: "right", label: "controls.dpad" },
  { id: "ls_up", label: "controls.ls" },
  { id: "ls_down", label: "controls.ls" },
  { id: "ls_left", label: "controls.ls" },
  { id: "ls_right", label: "controls.ls" },
  { id: "rs_up", label: "controls.rs" },
  { id: "rs_down", label: "controls.rs" },
  { id: "rs_left", label: "controls.rs" },
  { id: "rs_right", label: "controls.rs" },
];
const ARROW: Record<string, string> = { up: "↑", down: "↓", left: "←", right: "→" };

/** The game's built-in layout (the toolkit's pad_input_map.h, pim_defaults,
 * the same in every port of the toolkit): WASD, the mouse camera, Space for
 * Cross. */
export const DEFAULTS: Bindings = {
  keyboard: {
    cross: ["Space"], circle: ["C", "Control", "Backspace"], square: ["R", "E"], triangle: ["Q"],
    l1: ["Z"], r1: ["X"], l2: ["G"], r2: ["1"], l3: ["Shift"], r3: ["V"],
    start: ["Return", "KeypadEnter", "Escape", "P"], select: ["Tab", "M"],
    up: ["Up"], down: ["Down"], left: ["Left"], right: ["Right"],
    ls_up: ["W"], ls_down: ["S"], ls_left: ["A"], ls_right: ["D"],
    rs_up: ["I"], rs_down: ["K"], rs_left: ["J"], rs_right: ["L"],
  },
  mouseButtons: {
    r1: ["Left"], l1: ["Right"], r2: ["Middle"], l2: ["Button4"], square: ["Button5"], triangle: ["WheelUp", "WheelDown"],
  },
  controller: {
    cross: ["a"], circle: ["b"], square: ["x"], triangle: ["y"],
    l1: ["leftshoulder"], r1: ["rightshoulder"], l2: ["lefttrigger"], r2: ["righttrigger"],
    l3: ["leftstick"], r3: ["rightstick"], start: ["start"], select: ["back"],
    up: ["dpup"], down: ["dpdown"], left: ["dpleft"], right: ["dpright"],
  },
};

// ---- input names --------------------------------------------------------------
/** KeyboardEvent.code -> the game's key name (macOS key positions). */
function keyName(code: string): string | null {
  let m = /^Key([A-Z])$/.exec(code);
  if (m) return m[1];
  m = /^Digit([0-9])$/.exec(code);
  if (m) return m[1];
  m = /^F([0-9]{1,2})$/.exec(code);
  if (m && Number(m[1]) >= 1 && Number(m[1]) <= 20) return code;
  m = /^Numpad([0-9])$/.exec(code);
  if (m) return `Keypad${m[1]}`;
  const named: Record<string, string> = {
    Space: "Space", Enter: "Return", Tab: "Tab", Backspace: "Backspace", Delete: "ForwardDelete",
    ArrowUp: "Up", ArrowDown: "Down", ArrowLeft: "Left", ArrowRight: "Right",
    Home: "Home", End: "End", PageUp: "PageUp", PageDown: "PageDown", Insert: "Help", Help: "Help",
    Minus: "Minus", Equal: "Equal", BracketLeft: "LeftBracket", BracketRight: "RightBracket",
    Backslash: "Backslash", Semicolon: "Semicolon", Quote: "Quote", Comma: "Comma", Period: "Period",
    Slash: "Slash", Backquote: "Grave", IntlBackslash: "Section",
    NumpadEnter: "KeypadEnter", NumpadAdd: "KeypadPlus", NumpadSubtract: "KeypadMinus",
    NumpadMultiply: "KeypadMultiply", NumpadDivide: "KeypadDivide", NumpadDecimal: "KeypadDecimal",
    NumpadEqual: "KeypadEquals", NumLock: "KeypadClear",
    ShiftLeft: "Shift", ShiftRight: "RightShift", ControlLeft: "Control", ControlRight: "RightControl",
    AltLeft: "Option", AltRight: "RightOption",
  };
  return named[code] ?? null; // Cmd, Caps Lock, Fn, Esc: the game's own or not keys
}

/** MouseEvent.button -> the game's mouse button name. */
const MOUSE_BUTTONS = ["Left", "Middle", "Right", "Button4", "Button5"];
/** Gamepad standard mapping index -> SDL GameController name. */
const PAD_INDEX = [
  "a", "b", "x", "y", "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger",
  "back", "start", "leftstick", "rightstick", "dpup", "dpdown", "dpleft", "dpright", "guide", "touchpad",
];
/** SDL inputs offered in the list, with what they are on a PlayStation / Xbox pad. */
const PAD_INPUTS: [string, string][] = [
  ["a", "✕ / A"], ["b", "○ / B"], ["x", "□ / X"], ["y", "△ / Y"],
  ["leftshoulder", "L1 / LB"], ["rightshoulder", "R1 / RB"],
  ["lefttrigger", "L2 / LT"], ["righttrigger", "R2 / RT"],
  ["leftstick", "L3 / LS"], ["rightstick", "R3 / RS"],
  ["back", "Create, Share / View"], ["start", "Options / Menu"], ["guide", "PS / Xbox"],
  ["touchpad", "Touchpad"], ["misc1", "Mic / Share (Xbox)"],
  ["dpup", "D-pad ↑"], ["dpdown", "D-pad ↓"], ["dpleft", "D-pad ←"], ["dpright", "D-pad →"],
  ["paddle1", "Paddle 1"], ["paddle2", "Paddle 2"], ["paddle3", "Paddle 3"], ["paddle4", "Paddle 4"],
];
const PAD_LABEL = new Map(PAD_INPUTS);

/** Canonical spelling of a name read from the file (the game ignores case). */
const CANON = new Map<string, string>();
for (const d of DEVICES) for (const v of Object.values(DEFAULTS[d])) for (const n of v) CANON.set(n.toLowerCase(), n);
for (const n of [...MOUSE_BUTTONS, "Button6", "Button7", "Button8", "WheelUp", "WheelDown", "Escape"]) CANON.set(n.toLowerCase(), n);
for (const [n] of PAD_INPUTS) CANON.set(n, n);
const canon = (n: string) => CANON.get(n.toLowerCase()) ?? n;

function inputLabel(dev: Device, name: string): string {
  if (dev === "controller") return PAD_LABEL.get(name) ?? name;
  if (dev === "mouseButtons") {
    const k = `controls.mouse.${name}` as Key;
    const v = t(k);
    return v === k ? name : v;
  }
  return ({ Up: "↑", Down: "↓", Left: "←", Right: "→", Escape: "Esc" } as Record<string, string>)[name] ?? name;
}

// ---- state --------------------------------------------------------------------
const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;
let bindings: Bindings = clone(DEFAULTS);
let mapPath = "";
let mapExists = false;
let capture: { dev: Device; ctl: string; padBase: boolean[] } | null = null;
const heldKeys = new Set<string>();
const heldMouse = new Set<string>();
let heldPad = new Set<string>();
let padFrame = 0;

function clone(b: Bindings): Bindings {
  return {
    keyboard: Object.fromEntries(Object.entries(b.keyboard).map(([k, v]) => [k, [...v]])),
    mouseButtons: Object.fromEntries(Object.entries(b.mouseButtons).map(([k, v]) => [k, [...v]])),
    controller: Object.fromEntries(Object.entries(b.controller).map(([k, v]) => [k, [...v]])),
  };
}

/** Defaults with the file's entries on top: the full table the screen shows. */
function merged(file: Bindings): Bindings {
  const b = clone(DEFAULTS);
  for (const d of DEVICES) {
    for (const c of CONTROLS) b[d][c.id] ??= [];
    for (const [k, v] of Object.entries(file[d] ?? {})) b[d][k] = v.map(canon);
  }
  return b;
}

async function save() {
  try {
    mapPath = await invoke<string>("save_input_map", { bindings });
    mapExists = true;
    renderPath();
    flashSaved();
  } catch (e) {
    note(String(e));
  }
}

function note(msg: string) {
  $("controls-msg").textContent = msg;
}

/** Bind `name` to one control, taking it away from the others of that device. */
function bind(dev: Device, ctl: string, name: string) {
  const taken: string[] = [];
  for (const [k, v] of Object.entries(bindings[dev])) {
    if (k !== ctl && v.includes(name)) {
      bindings[dev][k] = v.filter((n) => n !== name);
      taken.push(controlLabel(k));
    }
  }
  bindings[dev][ctl] = [name];
  note(taken.length ? t("controls.moved", { input: inputLabel(dev, name), from: taken.join(", ") }) : "");
  endCapture();
  save();
}

function controlLabel(id: string): string {
  const c = CONTROLS.find((x) => x.id === id);
  if (!c) return id;
  const dir = id.replace(/^(ls_|rs_)/, "");
  return c.label.startsWith("controls.") ? `${t(c.label as Key)} ${ARROW[dir] ?? ""}`.trim() : c.label;
}

/** The file's path, or that there is none yet (the built-in layout). */
function renderPath() {
  $("controls-path").textContent = mapPath;
  $("controls-default").classList.toggle("hidden", mapExists);
}

// ---- the table ------------------------------------------------------------------
export function renderControls() {
  const body = $("controls-body");
  body.innerHTML = "";
  for (const c of CONTROLS) {
    const tr = document.createElement("tr");
    tr.dataset.ctl = c.id;
    const th = document.createElement("th");
    th.textContent = controlLabel(c.id);
    if (c.act) {
      const act = document.createElement("span");
      act.className = "act";
      act.textContent = t(c.act);
      th.appendChild(act);
    }
    tr.appendChild(th);
    for (const d of DEVICES) {
      const td = document.createElement("td");
      const cell = document.createElement("button");
      cell.className = "bind";
      const capturing = capture?.dev === d && capture.ctl === c.id;
      cell.classList.toggle("capturing", capturing);
      const names = bindings[d][c.id] ?? [];
      cell.textContent = capturing
        ? t(d === "keyboard" ? "controls.pressKey" : d === "mouseButtons" ? "controls.pressMouse" : "controls.pressPad")
        : names.length ? names.map((n) => inputLabel(d, n)).join(" / ") : "—";
      cell.title = names.join(", ");
      cell.addEventListener("click", (ev) => {
        ev.stopPropagation();
        if (capture?.dev === d && capture.ctl === c.id) return; // the capture takes this cell's own clicks
        startCapture(d, c.id);
      });
      td.appendChild(cell);
      if (capturing && d === "controller") {
        const sel = document.createElement("select");
        sel.className = "pad-pick";
        sel.append(new Option(t("controls.pickPad"), ""), ...PAD_INPUTS.map(([n, label]) => new Option(label, n)));
        sel.addEventListener("click", (ev) => ev.stopPropagation());
        sel.addEventListener("change", () => sel.value && bind("controller", c.id, sel.value));
        td.appendChild(sel);
      }
      if (names.length && !capturing) {
        const x = document.createElement("button");
        x.className = "unbind";
        x.textContent = "×";
        x.title = t("controls.unbind");
        x.addEventListener("click", (ev) => {
          ev.stopPropagation();
          bindings[d][c.id] = [];
          note("");
          renderControls();
          save();
        });
        td.appendChild(x);
      }
      tr.appendChild(td);
    }
    body.appendChild(tr);
  }
  renderHeld();
}

/** The rows whose inputs are held right now (the button test). */
function renderHeld() {
  document.querySelectorAll<HTMLTableRowElement>("#controls-body tr").forEach((tr) => {
    const id = tr.dataset.ctl!;
    const on =
      (bindings.keyboard[id] ?? []).some((n) => heldKeys.has(n)) ||
      (bindings.mouseButtons[id] ?? []).some((n) => heldMouse.has(n)) ||
      (bindings.controller[id] ?? []).some((n) => heldPad.has(n));
    tr.classList.toggle("held", on);
  });
}

function startCapture(dev: Device, ctl: string) {
  capture = { dev, ctl, padBase: padButtons() };
  note(t("controls.captureHint"));
  renderControls();
}

function endCapture() {
  capture = null;
  renderControls();
}

const visible = () => $("view-controls").classList.contains("active");

// ---- input events -------------------------------------------------------------------
window.addEventListener("keydown", (ev) => {
  if (!visible()) return;
  if (ev.metaKey) return; // Cmd shortcuts stay the app's
  const name = keyName(ev.code);
  if (capture) {
    ev.preventDefault();
    if (ev.code === "Escape") {
      note("");
      endCapture();
      return;
    }
    if (capture.dev !== "keyboard") return;
    if (!name) {
      note(t("controls.notBindable", { key: ev.code }));
      return;
    }
    bind("keyboard", capture.ctl, name);
    return;
  }
  if (name && !(ev.target instanceof HTMLInputElement || ev.target instanceof HTMLSelectElement)) {
    if (name === "Space" || name === "Return" || name.startsWith("Up") || name.startsWith("Down")) ev.preventDefault();
    heldKeys.add(name);
    renderHeld();
  }
}, true);
window.addEventListener("keyup", (ev) => {
  const name = keyName(ev.code);
  if (name && heldKeys.delete(name)) renderHeld();
}, true);
window.addEventListener("blur", () => {
  heldKeys.clear();
  heldMouse.clear();
  renderHeld();
});

function onMouseDown(ev: MouseEvent) {
  if (!visible()) return;
  const name = MOUSE_BUTTONS[ev.button];
  if (capture?.dev === "mouseButtons" && name) {
    ev.preventDefault();
    ev.stopPropagation();
    bind("mouseButtons", capture.ctl, name);
    return;
  }
  if (name && (ev.target as HTMLElement).closest("#controls-test")) {
    heldMouse.add(name);
    renderHeld();
  }
}
window.addEventListener("mousedown", onMouseDown, true);
window.addEventListener("mouseup", (ev) => {
  const name = MOUSE_BUTTONS[ev.button];
  if (name && heldMouse.delete(name)) renderHeld();
}, true);
window.addEventListener("contextmenu", (ev) => {
  if (visible() && (capture?.dev === "mouseButtons" || (ev.target as HTMLElement).closest("#controls-test"))) ev.preventDefault();
}, true);
window.addEventListener("wheel", (ev) => {
  if (!visible() || capture?.dev !== "mouseButtons" || ev.deltaY === 0) return;
  ev.preventDefault();
  bind("mouseButtons", capture.ctl, ev.deltaY < 0 ? "WheelUp" : "WheelDown");
}, { passive: false, capture: true });
// a click anywhere else ends a keyboard or controller capture
window.addEventListener("click", (ev) => {
  if (capture && capture.dev !== "mouseButtons" && !(ev.target as HTMLElement).closest(".pad-pick")) {
    note("");
    endCapture();
  }
});

/** The first connected pad, if the web view has the Gamepad API. */
function firstPad(): Gamepad | null {
  const pads = typeof navigator.getGamepads === "function" ? navigator.getGamepads() : [];
  for (const p of pads) if (p && p.connected) return p;
  return null;
}

/** The first connected standard-mapping pad's buttons. */
function padButtons(): boolean[] {
  return firstPad()?.buttons.map((b) => b.pressed || b.value > 0.5) ?? [];
}

/** The controller test: its name and where its sticks are. */
function renderPad(pad: Gamepad | null) {
  $("pad-name").textContent = pad
    ? t("controls.connected", { name: pad.id })
    : typeof navigator.getGamepads === "function" ? t("controls.noPad") : t("controls.noApi");
  const stick = (id: string, x = 0, y = 0) => {
    const dot = $(id).querySelector("i") as HTMLElement;
    dot.style.left = `${50 + x * 42}%`;
    dot.style.top = `${50 + y * 42}%`;
  };
  stick("stick-l", pad?.axes[0], pad?.axes[1]);
  stick("stick-r", pad?.axes[2], pad?.axes[3]);
}

function pollPad() {
  padFrame = 0;
  if (!visible()) return;
  renderPad(firstPad());
  const now = padButtons();
  const held = new Set<string>();
  now.forEach((down, i) => down && PAD_INDEX[i] && held.add(PAD_INDEX[i]));
  if (capture?.dev === "controller") {
    const fresh = now.findIndex((down, i) => down && !capture!.padBase[i] && PAD_INDEX[i]);
    if (fresh >= 0) {
      bind("controller", capture.ctl, PAD_INDEX[fresh]);
    } else {
      capture.padBase = capture.padBase.map((b, i) => b && !!now[i]); // released buttons can count next
    }
  }
  if ([...held].join() !== [...heldPad].join()) {
    heldPad = held;
    renderHeld();
  }
  padFrame = requestAnimationFrame(pollPad);
}

// ---- entry points -------------------------------------------------------------------
/** Called when the Controls view opens or closes. */
export async function showControls(on = true) {
  if (!on) {
    if (capture) endCapture();
    return;
  }
  try {
    const v = await invoke<InputMapView>("get_input_map");
    bindings = merged(v.bindings);
    mapPath = v.path;
    mapExists = v.exists;
  } catch (e) {
    note(String(e));
  }
  renderPath();
  renderControls();
  if (!padFrame) padFrame = requestAnimationFrame(pollPad);
}

export function initControls() {
  $("controls-reset").addEventListener("click", async () => {
    // no file is the built-in layout: Play passes no PS3_INPUT_MAP again
    try {
      await invoke("reset_input_map");
      mapExists = false;
      flashSaved();
    } catch (e) {
      note(String(e));
      return;
    }
    bindings = merged({ keyboard: {}, mouseButtons: {}, controller: {} });
    endCapture();
    note(t("controls.resetDone"));
    renderPath();
  });
  // without a file yet this shows the folder it will be written to
  $("controls-open").addEventListener("click", () => mapPath && invoke("reveal", { path: mapPath }));
  window.addEventListener("gamepadconnected", () => {
    if (visible() && !padFrame) padFrame = requestAnimationFrame(pollPad);
  });
}
