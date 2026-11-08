// Small pieces of UI shared by the screens.
import { t } from "./i18n";

let flashTimer = 0;

/** Every change is written at once (launcher.json, input.toml); this shows
 * it: the sidebar's dot and the page header's chip light up for a moment. */
export function flashSaved() {
  const state = document.getElementById("save-state");
  const text = document.getElementById("save-state-text");
  state?.classList.add("saving");
  if (text) text.textContent = t("settings.saved");
  document.querySelectorAll(".autosave-chip").forEach((c) => c.classList.add("flash"));
  window.clearTimeout(flashTimer);
  flashTimer = window.setTimeout(() => {
    state?.classList.remove("saving");
    if (text) text.textContent = t("ui.autosave");
    document.querySelectorAll(".autosave-chip").forEach((c) => c.classList.remove("flash"));
  }, 1200);
}

export function human(bytes: number): string {
  const units = ["B", "KB", "MB", "GB"];
  let v = bytes;
  let i = 0;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i++;
  }
  return `${v.toFixed(i && v < 10 ? 1 : 0)} ${units[i]}`;
}
