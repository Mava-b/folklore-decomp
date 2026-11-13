// Advanced > Updates (src-tauri/src/update.rs): the Folklore repository's
// sources (git fetch; behind its upstream, or newer than the build) and the
// launcher itself. The launcher has no release feed yet: its card says so
// and offers nothing to install (update.rs keeps the manifest check and the
// install path for when one exists). Home shows a bar when there is news.
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { Key, lang, t } from "./i18n";
import { human } from "./ui";

interface Sources {
  git: boolean;
  branch: string;
  commit: string;
  subject: string;
  commitTime: number;
  upstream: string;
  behind: number;
  ahead: number;
  dirty: number;
  sourcesTime: number;
  fetched: boolean;
  error: string | null;
}
interface AppUpdate {
  current: string;
  /** A release feed is configured (none yet). */
  feed: boolean;
  checked: boolean;
  latest: string | null;
  newer: boolean;
  url: string | null;
  notes: string | null;
  pubDate: string | null;
  disabled: boolean;
  error: string | null;
  installBlocker: string | null;
  installedPending: string | null;
  updatedFrom: string | null;
}
interface Progress {
  downloaded: number;
  total: number | null;
  phase: "download" | "install";
}
interface UpdateInfo {
  sources: Sources;
  rebuildNeeded: boolean;
  builtAt: number;
  app: AppUpdate;
}

const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;
let info: UpdateInfo | null = null;
let online = false;
let checking = false;
let opts: {
  auto: () => boolean;
  /** The game or a rebuild/install/pull is running (no app install, no restart then). */
  busy: () => "game" | "rebuild" | null;
  canRebuild: () => boolean;
  rebuild: () => void;
  pull: () => void;
};
/** The app install: idle, running (with its last progress), done, or failed. */
let install: { state: "idle" | "running" | "done" | "failed"; progress?: Progress; version?: string; error?: string } = {
  state: "idle",
};

const BLOCKERS: Record<string, Key> = {
  game: "update.blocked.game",
  rebuild: "update.blocked.rebuild",
  dmg: "update.blocked.dmg",
  translocated: "update.blocked.translocated",
  dev: "update.blocked.dev",
  platform: "update.blocked.platform",
  nofeed: "update.noFeed",
};

/** "blocked:<why>" errors from the backend, in words. */
function explain(e: unknown): string {
  const s = String(e);
  const m = /^blocked:(\w+)$/.exec(s);
  return m && BLOCKERS[m[1]] ? t(BLOCKERS[m[1]]) : s;
}

const show = (el: HTMLElement, on: boolean) => el.classList.toggle("hidden", !on);
const all = (sel: string) => Array.from(document.querySelectorAll<HTMLElement>(sel));

const date = (s: number) => (s ? new Date(s * 1000).toLocaleString(lang()) : "?");

/** `net`: also ask the network (git fetch, GitHub). */
export async function refreshUpdates(net: boolean) {
  if (checking) return;
  checking = true;
  renderUpdates();
  try {
    const r = await invoke<UpdateInfo>("check_updates", { online: net });
    // keep the last online answer when this was a local-only refresh
    const prev = info?.app;
    if (!net && prev?.checked) {
      Object.assign(r.app, {
        checked: prev.checked,
        latest: prev.latest,
        newer: prev.newer,
        url: prev.url,
        notes: prev.notes,
        pubDate: prev.pubDate,
        error: prev.error,
        installBlocker: r.app.installBlocker ?? prev.installBlocker,
      });
    }
    info = r;
    online = online || net;
  } catch {
    info = null;
  }
  checking = false;
  renderUpdates();
}

function line(cls: string, text: string): HTMLElement {
  const d = document.createElement("div");
  d.className = cls;
  d.textContent = text;
  return d;
}

export function renderUpdates() {
  if (!opts) return;
  const src = $("src-info");
  const app = $("app-info");
  src.replaceChildren();
  app.replaceChildren();
  $<HTMLButtonElement>("update-check").disabled = checking;
  $("update-check").textContent = t(checking ? "update.checking" : "update.check");
  const s = info?.sources;
  if (!info || !s) {
    src.append(line("small muted", t(checking ? "update.checking" : "update.notChecked")));
  } else if (!s.git) {
    src.append(line("small muted", t("update.noGit")));
  } else {
    src.append(line("", t("update.at", { branch: s.branch, commit: s.commit, subject: s.subject, date: date(s.commitTime) })));
    if (s.error) src.append(line("small warn-text", t("update.fetchFailed", { e: s.error })));
    if (!s.upstream) src.append(line("small muted", t("update.noUpstream")));
    else if (s.behind) src.append(line("warn-text", t("update.behind", { n: s.behind, up: s.upstream })));
    else if (online && s.fetched) src.append(line("ok-text small", t("update.upToDate", { up: s.upstream })));
    if (s.ahead) src.append(line("small muted", t("update.ahead", { n: s.ahead })));
    if (s.dirty) src.append(line("small muted", t("update.dirty", { n: s.dirty })));
    if (info.rebuildNeeded) src.append(line("warn-text", t("update.rebuildNeeded", { built: date(info.builtAt), src: date(s.sourcesTime) })));
  }
  const pull = $<HTMLButtonElement>("src-pull");
  pull.classList.toggle("hidden", !(s?.behind && !s.dirty));
  pull.disabled = !!opts.busy();
  const rb = $<HTMLButtonElement>("src-rebuild");
  rb.classList.toggle("hidden", !info?.rebuildNeeded);
  rb.disabled = !opts.canRebuild();

  const a = info?.app;
  const busy = opts.busy();
  const pending = install.state === "done" ? (install.version ?? null) : (a?.installedPending ?? null);
  const blocker = a?.installBlocker ?? null;
  const appNews: string[] = [];
  if (!a) app.append(line("small muted", t("update.notChecked")));
  else if (pending) {
    const l = t("update.installed", { v: pending });
    app.append(line("warn-text", l));
    appNews.push(l);
  } else if (a.disabled) app.append(line("small muted", t("update.disabled", { cur: a.current })));
  else if (a.newer) {
    let l = t("update.newRelease", { v: a.latest ?? "", cur: a.current });
    if (install.state === "failed") l += " " + t("update.installFailed", { e: install.error ?? "" });
    else if (install.state !== "running" && blocker) l += " " + t(BLOCKERS[blocker] ?? "update.blocked.platform");
    else if (install.state !== "running" && busy) l += " " + t(BLOCKERS[busy]);
    app.append(line("warn-text", l));
    appNews.push(l);
    if (a.notes) app.append(line("update-notes", a.notes.length > 1500 ? a.notes.slice(0, 1500) + "…" : a.notes));
  } else if (!online || !a.checked) app.append(line("small muted", t("update.appCurrent", { cur: a.current })));
  else if (a.error) app.append(line("small muted", t("update.appError", { cur: a.current, e: a.error })));
  else app.append(line("ok-text", t("update.appUpToDate", { cur: a.current })));
  // the restart is done: the new version offers the rebuild
  const after = !!a?.updatedFrom && !pending;
  if (after) {
    const l = t(s?.behind ? "update.afterUpdatePull" : "update.afterUpdate", { from: a?.updatedFrom ?? "", cur: a?.current ?? "" });
    app.append(line("warn-text", l));
    appNews.push(l);
  }
  const canRebuild = opts.canRebuild() && install.state !== "running";
  for (const id of ["app-rebuild", "home-update-rebuild"]) {
    const b = $<HTMLButtonElement>(id);
    show(b, id === "app-rebuild" ? after : after || !!info?.rebuildNeeded);
    b.disabled = !canRebuild;
  }
  const offerInstall = !!a?.newer && !a.disabled && !pending && !blocker;
  for (const id of ["app-install", "home-update-install"]) {
    const b = $<HTMLButtonElement>(id);
    show(b, offerInstall);
    b.disabled = install.state === "running" || !!busy;
  }
  for (const id of ["app-restart", "home-update-restart"]) {
    const b = $<HTMLButtonElement>(id);
    show(b, !!pending);
    b.disabled = !!busy;
  }
  // the release page: for a newer version that cannot be installed from here
  // (or did not install), and always in the card
  show($("app-open"), !!a?.newer && !!a.url && !pending);
  $<HTMLButtonElement>("update-check").disabled = checking || install.state === "running";

  // Home: one bar for whatever is new
  const news: string[] = [];
  if (s?.behind) news.push(t("update.behind", { n: s.behind, up: s.upstream }));
  if (info?.rebuildNeeded) news.push(t("update.rebuildShort"));
  news.push(...appNews);
  $("home-update").classList.toggle("hidden", !news.length && install.state !== "running");
  $("home-update-text").textContent = news.join(" ");
  renderProgress();
}

function progressText(): string {
  const p = install.progress;
  const v = install.version ?? info?.app.latest ?? "";
  if (!p || p.phase === "install") return t(p ? "update.installing" : "update.starting", { v });
  if (p.total) return t("update.downloading", { v, done: human(p.downloaded), total: human(p.total) });
  return t("update.downloadingNoTotal", { v, done: human(p.downloaded) });
}

function renderProgress() {
  const running = install.state === "running";
  for (const box of all(".update-progress")) show(box, running);
  if (!running) return;
  const p = install.progress;
  const frac = p?.phase === "download" && p.total ? Math.min(1, p.downloaded / p.total) : null;
  for (const bar of all(".update-bar")) {
    bar.classList.toggle("indeterminate", frac === null);
    bar.style.width = frac === null ? "" : `${(frac * 100).toFixed(1)}%`;
  }
  for (const el of all(".update-progress-text")) el.textContent = progressText();
}

async function startInstall() {
  if (install.state === "running" || !info?.app.newer) return;
  install = { state: "running", version: info.app.latest ?? undefined };
  renderUpdates();
  try {
    const v = await invoke<string>("install_update");
    install = { state: "done", version: v };
  } catch (e) {
    install = { state: "failed", error: explain(e) };
  }
  renderUpdates();
}

async function restart() {
  try {
    await invoke("restart_app");
  } catch (e) {
    $("home-update-text").textContent = explain(e);
  }
}

/** The launcher is installing its update (Play and the rebuild wait for it). */
export function updateRunning(): boolean {
  return install.state === "running";
}

export function initUpdates(o: typeof opts) {
  opts = o;
  $("update-check").addEventListener("click", () => refreshUpdates(true));
  $("src-pull").addEventListener("click", () => opts.pull());
  $("src-rebuild").addEventListener("click", () => opts.rebuild());
  $("app-open").addEventListener("click", () => info?.app.url && invoke("open_url", { url: info.app.url }));
  for (const id of ["app-install", "home-update-install"]) $(id).addEventListener("click", startInstall);
  for (const id of ["app-restart", "home-update-restart"]) $(id).addEventListener("click", restart);
  for (const id of ["app-rebuild", "home-update-rebuild"]) $(id).addEventListener("click", () => opts.rebuild());
  listen<Progress>("update-progress", (ev) => {
    if (install.state !== "running") return;
    install.progress = ev.payload;
    renderProgress();
  });
  $("home-update-go").addEventListener("click", () => {
    document.querySelector<HTMLElement>('.nav[data-view="advanced"]')?.click();
    document.querySelector<HTMLElement>('.tab[data-tab="updates"]')?.click();
  });
}
