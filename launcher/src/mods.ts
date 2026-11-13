// Graphics page: texture packs (folders of PNGs in <data>/mods, read by the
// game through PS3_MODS_DIR and PS3_MODS; the toolkit's rsx_draw_engine.c).
// The enabled ones are kept in the settings in priority order: the upper one
// wins when two replace the same texture. The dump (PS3_MODS_DUMP) is a
// setting of its own; its folder is <data>/texture-dump.
import { invoke } from "@tauri-apps/api/core";
import { t } from "./i18n";

interface ModPack {
  name: string;
  textures: number;
  description: string;
}
interface ModsInfo {
  dir: string;
  dumpDir: string;
  packs: ModPack[];
}

interface Host {
  enabled: () => string[];
  setEnabled: (names: string[]) => void;
}

let info: ModsInfo | null = null;
let host: Host | null = null;

function el<K extends keyof HTMLElementTagNameMap>(tag: K, cls?: string, text?: string): HTMLElementTagNameMap[K] {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}

/** The enabled packs that are still on disk, in their order. */
function enabledOnDisk(): string[] {
  const names = new Set((info?.packs ?? []).map((p) => p.name));
  return (host?.enabled() ?? []).filter((n) => names.has(n));
}

export function renderMods() {
  const list = document.getElementById("mods-list");
  if (!list || !host) return;
  list.replaceChildren();
  const packs = info?.packs ?? [];
  if (!packs.length) {
    list.append(el("li", "muted", t("mods.none")));
    return;
  }
  const on = enabledOnDisk();
  const order = [...on, ...packs.map((p) => p.name).filter((n) => !on.includes(n))];
  for (const name of order) {
    const p = packs.find((x) => x.name === name)!;
    const i = on.indexOf(name);
    const li = el("li");
    const label = el("label", "what");
    const box = el("input");
    box.type = "checkbox";
    box.checked = i >= 0;
    box.addEventListener("change", () => {
      const cur = enabledOnDisk();
      host!.setEnabled(box.checked ? [...cur.filter((n) => n !== name), name] : cur.filter((n) => n !== name));
      renderMods();
    });
    const text = el("span");
    text.append(el("b", undefined, p.name));
    const detail = [p.description, t("mods.textures", { n: p.textures })].filter(Boolean).join(" · ");
    text.append(el("span", "msg", detail));
    label.append(box, text);
    li.append(label);
    if (i >= 0) {
      const move = (d: number) => {
        const cur = enabledOnDisk();
        const j = cur.indexOf(name);
        const k = j + d;
        if (j < 0 || k < 0 || k >= cur.length) return;
        [cur[j], cur[k]] = [cur[k], cur[j]];
        host!.setEnabled(cur);
        renderMods();
      };
      const up = el("button", "link", "↑");
      up.title = t("mods.up");
      up.disabled = i === 0;
      up.addEventListener("click", () => move(-1));
      const down = el("button", "link", "↓");
      down.title = t("mods.down");
      down.disabled = i === on.length - 1;
      down.addEventListener("click", () => move(1));
      li.append(up, down);
    }
    list.append(li);
  }
}

export async function refreshMods() {
  try {
    info = await invoke<ModsInfo>("list_mods");
  } catch {
    info = null;
  }
  renderMods();
}

export function initMods(h: Host) {
  host = h;
  document.getElementById("mods-refresh")?.addEventListener("click", () => void refreshMods());
  document.getElementById("mods-open")?.addEventListener("click", async () => {
    if (!info) await refreshMods();
    if (info) void invoke("reveal", { path: info.dir });
  });
  document.getElementById("mods-dump-open")?.addEventListener("click", async () => {
    if (!info) await refreshMods();
    if (info) void invoke("reveal", { path: info.dumpDir });
  });
  window.addEventListener("focus", () => void refreshMods());
  void refreshMods();
}
