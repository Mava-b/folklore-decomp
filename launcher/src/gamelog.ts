// Advanced > Game log: the tail of the log the last game this launcher started
// wrote (last.log in the play folder, or the session folder's in debug mode), read by
// src-tauri's read_game_log. Followed every 2 s while the tab is open.
import { invoke } from "@tauri-apps/api/core";
import { lang, t } from "./i18n";
import { human } from "./ui";

interface GameLog {
  path: string;
  exists: boolean;
  size: number;
  modified: number;
  text: string;
  truncated: boolean;
}

const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;
/** What is read at most: the end of the file. */
const MAX_BYTES = 512 * 1024;
const MAX_LINES = 4000;
const WARN = /\b(warn(ing)?|stub|unimplemented|missing)\b/i;
const ERR = /\b(error|fail(ed|ure)?|fatal|abort(ed)?|panic|exception|crash(ed)?|assert(ion)?)\b/i;

let log: GameLog | null = null;
let timer = 0;
let lastKey = "";

const visible = () =>
  $("view-advanced").classList.contains("active") && $("tab-log").classList.contains("active");

function render() {
  const pre = $("glog");
  const state = $("glog-state");
  $("glog-path").textContent = log?.path ?? "";
  if (!log || !log.exists) {
    pre.replaceChildren();
    state.textContent = t("log.empty");
    return;
  }
  const filter = $<HTMLInputElement>("glog-filter").value.trim().toLowerCase();
  const errorsOnly = $<HTMLInputElement>("glog-errors").checked;
  let lines = log.text.split("\n");
  if (lines.length && lines[lines.length - 1] === "") lines.pop();
  if (filter) lines = lines.filter((l) => l.toLowerCase().includes(filter));
  if (errorsOnly) lines = lines.filter((l) => ERR.test(l) || WARN.test(l));
  const shown = lines.slice(-MAX_LINES);
  const atBottom = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 30;
  const frag = document.createDocumentFragment();
  for (const l of shown) {
    const cls = ERR.test(l) ? "l-err" : WARN.test(l) ? "l-warn" : "";
    if (cls) {
      const s = document.createElement("span");
      s.className = cls;
      s.textContent = l + "\n";
      frag.appendChild(s);
    } else {
      frag.appendChild(document.createTextNode(l + "\n"));
    }
  }
  pre.replaceChildren(frag);
  if (atBottom || !lastKey) pre.scrollTop = pre.scrollHeight;
  const when = log.modified ? new Date(log.modified * 1000).toLocaleString(lang()) : "";
  state.textContent =
    t("log.meta", { size: human(log.size), date: when, n: shown.length }) +
    (log.truncated ? ` ${t("log.truncated", { size: human(MAX_BYTES) })}` : "");
}

export async function refreshLog() {
  try {
    log = await invoke<GameLog>("read_game_log", { maxBytes: MAX_BYTES });
  } catch (e) {
    log = null;
    $("glog-state").textContent = String(e);
    return;
  }
  const key = `${log.path}|${log.size}|${log.modified}`;
  if (key !== lastKey) {
    render();
    lastKey = key;
  }
  follow();
}

/** Poll while the tab is open and "Follow" is on. */
function follow() {
  const on = visible() && $<HTMLInputElement>("glog-follow").checked;
  if (on && !timer) timer = window.setInterval(() => (visible() ? refreshLog() : follow()), 2000);
  if (!on && timer) {
    window.clearInterval(timer);
    timer = 0;
  }
}

export function showLog() {
  lastKey = "";
  refreshLog();
}

export function initLog() {
  $("glog-refresh").addEventListener("click", showLog);
  $("glog-follow").addEventListener("change", follow);
  $("glog-filter").addEventListener("input", render);
  $("glog-errors").addEventListener("change", render);
  $("glog-copy").addEventListener("click", () => navigator.clipboard.writeText($("glog").textContent ?? ""));
  $("glog-open").addEventListener("click", () => log && invoke("reveal", { path: log.path }));
}

export function renderLog() {
  render();
}
