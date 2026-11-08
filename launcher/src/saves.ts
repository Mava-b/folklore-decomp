// Game > Saves: Folklore's save slots (BCUS98147… / BCES00050… folders) in
// the repository's gamedata/ and hdd0/ save folders, with backups, restore,
// delete and .zip export/import (src-tauri/src/saves.rs). Every destructive
// action asks first and backs up.
import { invoke } from "@tauri-apps/api/core";
import { ask, open, save } from "@tauri-apps/plugin-dialog";
import { Key, lang, t } from "./i18n";
import { human } from "./ui";

interface Slot {
  key: string;
  name: string;
  root: string;
  title: string;
  subtitle: string;
  detail: string;
  modified: number;
  bytes: number;
  path: string;
  icon: string | null;
}
interface BackupMeta {
  id: string;
  created: number;
  reason: string;
  slots: string[];
  titles: Record<string, string>;
  bytes: number;
}
interface Overview {
  slots: Slot[];
  backups: BackupMeta[];
  dirs: [string, string][];
  backupsDir: string;
  gameRunning: boolean;
}

const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;
const tilde = (p: string) => p.replace(/^\/Users\/[^/]+/, "~");
let ov: Overview | null = null;
let busy = false;
let opts: { keep: () => number; setKeep: (n: number) => void };

function el<K extends keyof HTMLElementTagNameMap>(tag: K, cls = "", text = ""): HTMLElementTagNameMap[K] {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text) e.textContent = text;
  return e;
}

function msg(text: string) {
  $("saves-msg").textContent = text;
}

export async function refresh() {
  try {
    ov = await invoke<Overview>("saves_overview");
  } catch (e) {
    msg(String(e));
  }
  render();
}

function slotName(s: Slot): string {
  return s.subtitle || s.title || s.name;
}

async function run<T>(what: () => Promise<T>, done: (r: T) => string) {
  if (busy) return;
  busy = true;
  render();
  try {
    msg(done(await what()));
  } catch (e) {
    msg(String(e));
  }
  busy = false;
  await refresh();
}

function slotCard(s: Slot): HTMLElement {
  const card = el("div", "slot");
  if (s.icon) {
    const img = el("img", "slot-icon");
    img.src = s.icon;
    img.alt = "";
    card.append(img);
  }
  const body = el("div", "slot-body");
  body.append(el("strong", "", slotName(s)));
  if (s.detail) body.append(el("span", "small muted", s.detail));
  body.append(el("span", "small muted", `${s.name} · ${s.root} · ${human(s.bytes)} · ${s.modified ? new Date(s.modified * 1000).toLocaleString(lang()) : "?"}`));
  const act = el("div", "row slot-actions");
  const b = (label: string, fn: () => void, cls = "") => {
    const x = el("button", cls, label);
    x.disabled = busy || !!ov?.gameRunning;
    x.addEventListener("click", fn);
    act.append(x);
  };
  b(t("saves.backup"), () => run(() => invoke<BackupMeta | null>("saves_backup", { key: s.key }), (r) => (r ? t("saves.backedUp", { id: r.id }) : "")));
  b(t("saves.reveal"), () => invoke("reveal", { path: s.path }), "link");
  b(t("saves.delete"), async () => {
    const yes = await ask(t("saves.deleteAsk", { name: slotName(s) }), { title: t("saves.delete"), kind: "warning" });
    if (yes) run(() => invoke<BackupMeta>("saves_delete_slot", { key: s.key }), (r) => t("saves.deleted", { id: r.id }));
  }, "link danger-link");
  body.append(act);
  card.append(body);
  return card;
}

const REASONS: Record<string, Key> = {
  auto: "saves.reason.auto",
  manual: "saves.reason.manual",
  "before-restore": "saves.reason.restore",
  "before-import": "saves.reason.import",
  "before-delete": "saves.reason.delete",
};

export function render() {
  if (!opts) return;
  const keep = $<HTMLInputElement>("saves-keep");
  if (document.activeElement !== keep) keep.value = String(opts.keep());
  if (!ov) return;
  $("saves-busy").classList.toggle("hidden", !ov.gameRunning);
  for (const id of ["saves-backup-all", "saves-export-zip", "saves-import-zip"]) $<HTMLButtonElement>(id).disabled = busy || ov.gameRunning;
  $("saves-list").replaceChildren(...(ov.slots.length ? ov.slots.map(slotCard) : [el("p", "small muted", t(ov.dirs.length ? "saves.none" : "saves.noRepo"))]));
  $("saves-dirs").textContent = ov.dirs.length ? t("saves.dirs", { dirs: ov.dirs.map(([l, p]) => `${l}: ${tilde(p)}`).join(" · ") }) : "";
  const ul = $("saves-backups");
  ul.replaceChildren();
  if (!ov.backups.length) ul.append(el("li", "small muted", t("saves.noBackups")));
  for (const bk of ov.backups) {
    const li = el("li");
    const what = el("div", "what");
    const when = new Date(bk.created * 1000).toLocaleString(lang());
    what.append(el("b", "", when));
    const names = bk.slots.map((s) => bk.titles[s] || s.split("/").pop() || s).join(", ");
    what.append(el("span", "msg muted", `${t(REASONS[bk.reason] ?? "saves.reason.manual")} · ${names} · ${human(bk.bytes)}`));
    li.append(what);
    const restore = el("button", "", t("saves.restore"));
    restore.disabled = busy || ov.gameRunning;
    restore.addEventListener("click", async () => {
      const yes = await ask(t("saves.restoreAsk", { when }), { title: t("saves.restore"), kind: "warning" });
      if (yes) run(() => invoke<{ backup: string | null }>("saves_restore", { id: bk.id, key: null }), (r) => t("saves.restored") + (r.backup ? " " + t("saves.backedUp", { id: r.backup }) : ""));
    });
    const del = el("button", "link danger-link", t("saves.delete"));
    del.disabled = busy;
    del.addEventListener("click", async () => {
      const yes = await ask(t("saves.deleteBackupAsk", { when }), { title: t("saves.delete"), kind: "warning" });
      if (yes) run(() => invoke("saves_delete_backup", { id: bk.id }), () => "");
    });
    li.append(restore, del);
    ul.append(li);
  }
}

export function initSaves(o: typeof opts) {
  opts = o;
  $("saves-refresh").addEventListener("click", refresh);
  $("saves-keep").addEventListener("change", () => {
    const n = Math.max(0, Math.min(100, Math.round(Number($<HTMLInputElement>("saves-keep").value) || 0)));
    opts.setKeep(n);
    render();
  });
  $("saves-backup-all").addEventListener("click", () =>
    run(() => invoke<BackupMeta | null>("saves_backup", { key: null }), (r) => (r ? t("saves.backedUp", { id: r.id }) : t("saves.none"))));
  $("saves-export-zip").addEventListener("click", async () => {
    const dest = await save({ title: t("saves.exportZip"), defaultPath: "folklore-saves.zip", filters: [{ name: "ZIP", extensions: ["zip"] }] });
    if (dest) run(() => invoke<string>("saves_export_zip", { keys: null, dest }), (p) => t("saves.exported", { path: tilde(p) }));
  });
  $("saves-import-zip").addEventListener("click", async () => {
    const p = await open({ title: t("saves.importZip"), filters: [{ name: "ZIP", extensions: ["zip"] }] });
    if (typeof p !== "string") return;
    const yes = await ask(t("saves.importAsk"), { title: t("saves.importZip"), kind: "warning" });
    if (yes) run(() => invoke<{ slots: string[]; backup: string | null }>("saves_import_zip", { path: p }), (r) => t("saves.imported", { n: r.slots.length }) + (r.backup ? " " + t("saves.backedUp", { id: r.backup }) : ""));
  });
  $("saves-open").addEventListener("click", () => ov?.dirs[0] && invoke("reveal", { path: ov.dirs[0][1] }));
  $("saves-open-backups").addEventListener("click", () => ov && invoke("reveal", { path: ov.backupsDir }));
}
