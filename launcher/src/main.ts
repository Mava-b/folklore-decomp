import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { ask, open } from "@tauri-apps/plugin-dialog";
import { Key, Lang, lang, setLang, t, translateDom } from "./i18n";
import { initControls, renderControls, showControls } from "./controls";
import { RebuildProgress, StepId } from "./progress";
import { initSaves, refresh as refreshSaves, render as renderSaves } from "./saves";
import { initUpdates, refreshUpdates, renderUpdates } from "./update";
import { initLog, renderLog, showLog } from "./gamelog";
import { initMods, refreshMods, renderMods } from "./mods";
import { flashSaved, human } from "./ui";

// ---- types shared with src-tauri/src/lib.rs (camelCase) ----------------------
export interface Settings {
  uiLang: string;
  repoDir: string;
  gamePath: string;
  mountForArt: boolean;
  windowMode: string;
  windowSize: string;
  display: string;
  upscale: string;
  hostWindow: boolean;
  vsync: boolean;
  resScale: string;
  gameIcon: boolean;
  aa: string;
  sharpen: number;
  aniso: number;
  gammaOverride: boolean;
  gamma: number;
  vibrance: number;
  contrast: number;
  vignette: number;
  grain: number;
  lutPath: string;
  lutStrength: number;
  mods: string[];
  modsDump: boolean;
  inputMode: string;
  mouse: boolean;
  mouseSens: number;
  mouseInvertY: boolean;
  mouseInvertX: boolean;
  mouseDeadzone: number;
  mouseCapture: boolean;
  rumble: boolean;
  rumbleStrength: number;
  audioVolume: number;
  audioMute: boolean;
  audioDevice: string;
  showFps: boolean;
  overlay: boolean;
  overlayHud: boolean;
  overlayHudCorner: number;
  vertexCache: boolean;
  vertexCacheSplit: boolean;
  bindChangedOnly: boolean;
  shaderCache: boolean;
  shaderAsync: boolean;
  keepAwake: boolean;
  video: boolean;
  frameRateCap: string;
  fineLimiterSleep: boolean;
  physicsFrameStep: boolean;
  logoSkip: string;
  motionControls: boolean;
  language: string;
  quietLog: boolean;
  extraEnv: string;
  jobs: number;
  noPromote: boolean;
  relift: boolean;
  saveBackupsKeep: number;
  autoUpdateCheck: boolean;
}
interface Builds {
  root: string;
  exe: string | null;
  host: string | null;
  builtAt: number;
  ok: boolean;
  venv: boolean;
  unself: boolean;
  decrypted: boolean;
  lifted: boolean;
  region: string;
  missing: string[];
}
interface GameInfo {
  envFile: boolean;
  path: string | null;
  exists: boolean;
  isImage: boolean;
  dir: string | null;
  title: string;
  titleId: string;
  appVer: string;
  ours: boolean;
}
interface SaveInfo {
  name: string;
  title: string;
  subtitle: string;
  detail: string;
  modified: number;
  folder: string;
}
interface Status {
  repo: string | null;
  repoInvalid: string | null;
  builds: Builds | null;
  game: GameInfo;
  icon: string | null;
  background: string | null;
  lastSave: SaveInfo | null;
  gameRunning: boolean;
  otherGameRunning: boolean;
  rebuildRunning: boolean;
  overlayIni: { path: string; exists: boolean; keys: string[] } | null;
  dataRoot: string;
  hostSupported: boolean;
  os: string;
  appBundle: string | null;
  finderIcon: boolean;
  version: string;
  /** The feature markers (variable names) the game's executable contains. */
  features: string[];
}
interface CheckItem {
  level: "ok" | "warn" | "fail" | "info";
  name: string;
  /** English fallback for `key`. */
  message: string;
  /** i18n key under prereq.msg. ("" = the detail is only the path). */
  key: string;
  vars: Record<string, string>;
  path: string | null;
}
interface Prereqs {
  supported: boolean;
  clt: boolean;
  brew: boolean;
  checks: CheckItem[];
  ok: boolean;
  repo: string | null;
  game: boolean;
  buildsOk: boolean;
  freeGb: number | null;
  needGb: number;
}

const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;
const show = (el: HTMLElement, on: boolean) => el.classList.toggle("hidden", !on);
const tilde = (p: string) => p.replace(/^\/Users\/[^/]+/, "~");
const URL_ISSUE = "https://github.com/erikomis/folklore-decomp/issues/new";
const BREW_URL = "https://brew.sh/";
const BREW_CMD = '/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"';
/** The releases the Mac build runs. */
const REGIONS: Record<string, Key> = { BCUS98147: "region.us", BCES00050: "region.eu", "1.10": "region.update" };

let settings: Settings;
let status: Status | null = null;
let gameRunning = false;
let rebuildRunning = false;
const progress = new RebuildProgress();

function el<K extends keyof HTMLElementTagNameMap>(tag: K, cls = "", text = ""): HTMLElementTagNameMap[K] {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text) e.textContent = text;
  return e;
}

function clock(secs: number | null): string {
  if (secs == null) return "-";
  const s = Math.round(secs);
  const h = Math.floor(s / 3600);
  const mm = String(Math.floor((s % 3600) / 60)).padStart(h ? 2 : 1, "0");
  const ss = String(s % 60).padStart(2, "0");
  return h ? `${h}:${mm}:${ss}` : `${mm}:${ss}`;
}

// ---- navigation ----------------------------------------------------------------
let advancedTab = "install";
const VIEWS = ["home", "graphics", "performance", "game", "controls", "audio", "advanced"];

function showView(view: string, tab?: string) {
  document.querySelectorAll<HTMLElement>(".view").forEach((v) => v.classList.toggle("active", v.id === `view-${view}`));
  document.querySelectorAll<HTMLElement>(".nav").forEach((b) => b.classList.toggle("active", b.dataset.view === view));
  document.querySelector("main")!.scrollTop = 0;
  if (view === "home") refreshStatus();
  if (view === "graphics") {
    refreshMods();
    refreshScreens();
  }
  if (view === "performance") refreshShaderCache();
  if (view === "audio") refreshAudioOutputs();
  if (view === "game") {
    refreshStatus();
    refreshSaves();
  }
  showControls(view === "controls");
  if (view === "advanced") showTab(tab ?? advancedTab);
}

function showTab(tab: string) {
  advancedTab = tab;
  document.querySelectorAll<HTMLElement>(".tab").forEach((b) => b.classList.toggle("active", b.dataset.tab === tab));
  document.querySelectorAll<HTMLElement>(".tab-panel").forEach((p) => p.classList.toggle("active", p.id === `tab-${tab}`));
  if (tab === "env") updatePreview();
  if (tab === "log") showLog();
  if (tab === "updates") renderUpdates();
  if (tab === "install") {
    refreshPrereqs();
    renderPort();
    renderRun();
  }
}

document.querySelectorAll<HTMLElement>(".nav").forEach((b) => b.addEventListener("click", () => showView(b.dataset.view!)));
document.querySelectorAll<HTMLElement>(".tab").forEach((b) => b.addEventListener("click", () => showTab(b.dataset.tab!)));
document.querySelectorAll<HTMLElement>("[data-go]").forEach((b) =>
  b.addEventListener("click", () => {
    const [view, tab] = b.dataset.go!.split(":");
    showView(view, tab);
  }));

// ---- settings persistence --------------------------------------------------------
let saveTimer = 0;
function persist(after?: () => void) {
  window.clearTimeout(saveTimer);
  saveTimer = window.setTimeout(async () => {
    try {
      await invoke("save_settings", { settings });
      flashSaved();
    } catch (e) {
      $("home-msg").textContent = String(e);
    }
    updatePreview();
    after?.();
  }, 250);
}

async function updatePreview() {
  try {
    $("preview").textContent = await invoke<string>("launch_preview", { settings });
  } catch (e) {
    $("preview").textContent = String(e);
  }
}

// ---- shader cache ----------------------------------------------------------------
interface ShaderCacheInfo {
  path: string | null;
  bytes: number;
  files: number;
}
async function refreshShaderCache(msg = "") {
  try {
    const c = await invoke<ShaderCacheInfo>("shader_cache_info");
    $("sc-info").textContent =
      (c.path ? t("sc.size", { size: human(c.bytes), files: c.files, path: tilde(c.path) }) : t("sc.off")) + (msg ? ` — ${msg}` : "");
    $<HTMLButtonElement>("sc-clear").disabled = !c.path || c.files === 0;
  } catch (e) {
    $("sc-info").textContent = String(e);
  }
}
$("sc-clear").addEventListener("click", async () => {
  try {
    await invoke("shader_cache_clear");
    refreshShaderCache(t("sc.cleared"));
  } catch (e) {
    refreshShaderCache(String(e));
  }
});

/** A setting can have more than one control (Home's quick settings repeat a
 * few of the pages'): after every change each control is set again. */
const syncers: (() => void)[] = [];
function syncAll() {
  for (const f of syncers) f();
  syncDependent();
}

const WINDOW_SIZES = ["", "1600x900", "1920x1080", "2560x1440", "3840x2160"];

function fillFixedSelects() {
  const fill = (sel: string, items: [string, string][]) =>
    document.querySelectorAll<HTMLSelectElement>(sel).forEach((s) => s.replaceChildren(...items.map(([v, l]) => new Option(l, v))));
  fill("select.size-select", WINDOW_SIZES.map((v) => [v, v ? v.replace("x", " × ") : t("settings.windowSizeDefault")]));
  fill("select.mode-select", [["windowed", t("settings.windowed")], ["fullscreen", t("settings.fullscreen")], ["borderless", t("settings.borderless")]]);
  fill("select.upscale-select", [["", t("settings.upscaleOff")], ["metalfx", "MetalFX"]]);
  fill("select.cap-select", [["original", t("settings.capOriginal")], ["60", t("settings.cap60")], ["unlimited", t("settings.capUnlimited")]]);
}

type BoolKey = { [K in keyof Settings]: Settings[K] extends boolean ? K : never }[keyof Settings];
type NumKey = { [K in keyof Settings]: Settings[K] extends number ? K : never }[keyof Settings];
type StrKey = { [K in keyof Settings]: Settings[K] extends string ? K : never }[keyof Settings];

function bindCheck(id: string, key: BoolKey, after?: () => void) {
  const elx = $<HTMLInputElement>(id);
  const sync = () => (elx.checked = settings[key]);
  syncers.push(sync);
  sync();
  elx.addEventListener("change", () => {
    settings[key] = elx.checked;
    syncAll();
    persist(key === "gameIcon" || key === "mountForArt" ? () => refreshStatus() : after);
  });
}

function bindRange(id: string, key: NumKey, fmt: (v: number) => string) {
  const elx = $<HTMLInputElement>(id);
  const out = $(`${id}-out`);
  const sync = () => {
    elx.value = String(settings[key]);
    out.textContent = fmt(Number(elx.value));
  };
  syncers.push(sync);
  sync();
  elx.addEventListener("input", () => {
    settings[key] = Number(elx.value);
    out.textContent = fmt(Number(elx.value));
    syncPreset();
    persist();
  });
}

function bindSelect(id: string, key: StrKey | "aniso" | "overlayHudCorner") {
  const elx = $<HTMLSelectElement>(id);
  const sync = () => {
    elx.value = String(settings[key]);
    if (elx.value !== String(settings[key])) elx.selectedIndex = 0;
  };
  syncers.push(sync);
  sync();
  elx.addEventListener("change", () => {
    if (key === "aniso" || key === "overlayHudCorner") settings[key] = Number(elx.value);
    else settings[key] = elx.value;
    syncAll();
    persist();
  });
}

function bindText(id: string, key: StrKey, after?: () => void) {
  const elx = $<HTMLInputElement | HTMLTextAreaElement>(id);
  const sync = () => (elx.value = settings[key]);
  syncers.push(sync);
  sync();
  elx.addEventListener("change", () => {
    settings[key] = elx.value.trim();
    persist(after);
  });
}

/** The ready-made image profiles: Performance (the PS3's own picture, the
 * cheapest), Balanced (1080p inside, FXAA, a little sharpening, 4x
 * anisotropic) and Quality (1440p inside, SMAA, 16x anisotropic, MetalFX to
 * the window's size, as the inFAMOUS 2 launcher's Quality). The list shows
 * the one the values match, else Custom. */
const PRESETS: Record<string, Pick<Settings, "resScale" | "aniso" | "aa" | "sharpen" | "upscale">> = {
  performance: { resScale: "", aniso: 0, aa: "", sharpen: 0, upscale: "" },
  balanced: { resScale: "1.5", aniso: 4, aa: "fxaa", sharpen: 0.2, upscale: "" },
  quality: { resScale: "2", aniso: 16, aa: "smaa", sharpen: 0.3, upscale: "metalfx" },
};
function currentPreset(): string {
  for (const [name, p] of Object.entries(PRESETS)) {
    if (
      settings.resScale === p.resScale &&
      settings.aniso === p.aniso &&
      settings.aa === p.aa &&
      settings.upscale === p.upscale &&
      Math.abs(settings.sharpen - p.sharpen) < 0.005
    )
      return name;
  }
  return "custom";
}
function syncPreset() {
  $<HTMLSelectElement>("preset").value = currentPreset();
}

// ---- screens (PS3_DISPLAY) --------------------------------------------------------
let screens: string[] = [];
async function refreshScreens() {
  screens = await invoke<string[]>("list_screens").catch(() => []);
  fillScreens();
}
/** The main screen (the default), the connected ones, and the saved one when
 * it is not connected now. */
function fillScreens() {
  const sel = $<HTMLSelectElement>("display");
  sel.replaceChildren(new Option(t("settings.screenMain"), ""));
  for (const n of screens) sel.append(new Option(n, n));
  const cur = settings.display;
  if (cur && !screens.includes(cur)) sel.append(new Option(t("settings.screenMissing", { name: cur }), cur));
  sel.value = cur;
}

// ---- sound outputs (PS3_AUDIO_DEVICE) ---------------------------------------------
let audioOutputs: string[] = [];
async function refreshAudioOutputs() {
  audioOutputs = await invoke<string[]>("list_audio_outputs").catch(() => []);
  fillAudioOutputs();
}
/** The system's default output, the ones the Mac has now, and the saved one
 * when it is not connected. */
function fillAudioOutputs() {
  const sel = $<HTMLSelectElement>("audio-device");
  sel.replaceChildren(new Option(t("settings.audioDeviceDefault"), ""));
  for (const n of audioOutputs) sel.append(new Option(n, n));
  const cur = settings.audioDevice;
  if (cur && !audioOutputs.includes(cur)) sel.append(new Option(t("settings.screenMissing", { name: cur }), cur));
  sel.value = cur;
}

// ---- what the build supports ------------------------------------------------------
/** Sections whose feature the build lacks say so ("rebuild"). */
function renderFeatures() {
  const have = new Set(status?.features ?? []);
  const known = !!status?.builds?.exe; // no build yet: nothing to say
  document.querySelectorAll<HTMLElement>("[data-feature]").forEach((sec) => {
    const missing = known && !have.has(sec.dataset.feature!);
    let note = sec.querySelector<HTMLElement>(":scope > .needs-rebuild");
    if (missing && !note) {
      note = el("p", "hint hint-warn needs-rebuild");
      const head = sec.querySelector(":scope > h2, :scope > .section-head");
      if (head) head.after(note);
      else sec.querySelector(".opt-text")?.append(note);
    }
    if (note) {
      note.textContent = t("feature.rebuild");
      show(note, missing);
    }
  });
}

function bindSettings() {
  fillFixedSelects();
  bindCheck("game-icon", "gameIcon");
  bindCheck("mount-art", "mountForArt");
  bindCheck("gamma-on", "gammaOverride");
  bindCheck("show-fps", "showFps");
  bindCheck("q-fps", "showFps");
  bindCheck("overlay", "overlay");
  bindCheck("overlay-hud", "overlayHud");
  bindCheck("keep-awake", "keepAwake");
  bindCheck("vcache", "vertexCache");
  bindCheck("vcache-split", "vertexCacheSplit");
  bindCheck("bind-changed", "bindChangedOnly");
  bindCheck("shader-cache", "shaderCache", () => refreshShaderCache());
  bindCheck("shader-async", "shaderAsync");
  bindCheck("video", "video");
  bindCheck("quiet-log", "quietLog");
  bindCheck("update-auto", "autoUpdateCheck");
  bindCheck("vsync", "vsync");
  bindCheck("mods-dump", "modsDump");
  bindCheck("mouse-on", "mouse");
  bindCheck("mouse-invert", "mouseInvertY");
  bindCheck("mouse-invert-x", "mouseInvertX");
  bindCheck("mouse-capture", "mouseCapture");
  bindCheck("rumble-on", "rumble");
  bindCheck("audio-mute", "audioMute");
  bindCheck("physics-step", "physicsFrameStep");
  bindCheck("fine-sleep", "fineLimiterSleep");
  bindCheck("motion", "motionControls");
  bindCheck("no-promote", "noPromote");
  bindCheck("relift", "relift");
  bindSelect("window-mode", "windowMode");
  bindSelect("q-window-mode", "windowMode");
  bindSelect("window-size", "windowSize");
  bindSelect("display", "display");
  bindSelect("upscale", "upscale");
  bindSelect("q-upscale", "upscale");
  bindSelect("q-window-size", "windowSize");
  bindSelect("fps-cap", "frameRateCap");
  bindSelect("q-fps-cap", "frameRateCap");
  bindSelect("res-scale", "resScale");
  bindSelect("input-mode", "inputMode");
  bindSelect("aa", "aa");
  bindSelect("aniso", "aniso");
  bindSelect("hud-corner", "overlayHudCorner");
  bindSelect("logo-skip", "logoSkip");
  bindSelect("language", "language");
  bindRange("sharpen", "sharpen", (v) => (v > 0 ? `${Math.round(v * 100)}%` : t("settings.off")));
  bindRange("gamma", "gamma", (v) => v.toFixed(2));
  const signedPct = (v: number) => (Math.abs(v) < 0.005 ? t("settings.off") : `${v > 0 ? "+" : ""}${Math.round(v * 100)}%`);
  const pct = (v: number) => (v > 0.005 ? `${Math.round(v * 100)}%` : t("settings.off"));
  bindRange("vibrance", "vibrance", signedPct);
  bindRange("contrast", "contrast", (v) => (Math.abs(v - 1) < 0.005 ? t("settings.off") : `${Math.round(v * 100)}%`));
  bindRange("vignette", "vignette", pct);
  bindRange("grain", "grain", pct);
  bindRange("lut-strength", "lutStrength", (v) => `${Math.round(v * 100)}%`);
  bindRange("mouse-sens", "mouseSens", (v) => `${v.toFixed(2)}×`);
  bindRange("mouse-dz", "mouseDeadzone", (v) => String(v));
  bindRange("rumble-strength", "rumbleStrength", (v) => `${v}%`);
  bindRange("audio-volume", "audioVolume", (v) => `${v}%`);
  const lutSync = () => {
    $("lut-file").textContent = settings.lutPath ? tilde(settings.lutPath) : t("settings.lutNone");
  };
  syncers.push(lutSync);
  lutSync();
  $("lut-pick").addEventListener("click", async () => {
    const p = await open({ title: t("settings.lut"), filters: [{ name: "PNG", extensions: ["png"] }] });
    if (typeof p === "string") {
      settings.lutPath = p;
      syncAll();
      persist();
    }
  });
  $("lut-clear").addEventListener("click", () => {
    settings.lutPath = "";
    syncAll();
    persist();
  });
  const own = $<HTMLInputElement>("own-window");
  const ownSync = () => (own.checked = !settings.hostWindow);
  syncers.push(ownSync);
  ownSync();
  own.addEventListener("change", () => {
    settings.hostWindow = !own.checked;
    syncAll();
    persist();
  });
  bindText("extra-env", "extraEnv");
  bindSelect("audio-device", "audioDevice");
  syncers.push(syncPreset);
  syncPreset();
  $("preset").addEventListener("change", () => {
    const p = PRESETS[$<HTMLSelectElement>("preset").value];
    if (!p) return;
    Object.assign(settings, p);
    syncAll();
    persist();
  });
  // the extra variables are saved as they are typed
  $("extra-env").addEventListener("input", () => {
    settings.extraEnv = $<HTMLTextAreaElement>("extra-env").value;
    persist();
  });
  $("finder-icon").addEventListener("click", toggleFinderIcon);
  const ui = $<HTMLSelectElement>("ui-lang");
  ui.value = settings.uiLang;
  ui.addEventListener("change", () => {
    settings.uiLang = ui.value;
    applyLang(ui.value as Lang);
    persist();
  });
  $("jobs").addEventListener("change", () => {
    settings.jobs = Math.max(0, Math.min(64, Number($<HTMLInputElement>("jobs").value) || 0));
    persist();
  });
  syncers.push(() => {
    const j = $<HTMLInputElement>("jobs");
    if (document.activeElement !== j) j.value = String(settings.jobs || 0);
  });
  $("reset").addEventListener("click", async () => {
    const yes = await ask(t("settings.resetAsk"), { title: t("settings.reset"), kind: "warning" });
    if (!yes) return;
    const keep: Partial<Settings> = {
      uiLang: settings.uiLang,
      repoDir: settings.repoDir,
      gamePath: settings.gamePath,
      saveBackupsKeep: settings.saveBackupsKeep,
    };
    Object.assign(settings, DEFAULTS, keep);
    await invoke("save_settings", { settings });
    location.reload();
  });
  syncDependent();
}

/** lib.rs Settings::default() for the options the Restore button resets. */
const DEFAULTS: Partial<Settings> = {
  mountForArt: true,
  windowMode: "windowed",
  windowSize: "",
  display: "",
  upscale: "",
  hostWindow: true,
  vsync: true,
  resScale: "",
  gameIcon: false,
  aa: "",
  sharpen: 0,
  aniso: 0,
  gammaOverride: false,
  gamma: 1,
  vibrance: 0,
  contrast: 1,
  vignette: 0,
  grain: 0,
  lutPath: "",
  lutStrength: 1,
  mods: [],
  modsDump: false,
  inputMode: "both",
  mouse: true,
  mouseSens: 1,
  mouseInvertY: false,
  mouseInvertX: false,
  mouseDeadzone: 30,
  mouseCapture: true,
  rumble: true,
  rumbleStrength: 100,
  audioVolume: 100,
  audioMute: false,
  audioDevice: "",
  showFps: true,
  overlay: true,
  overlayHud: true,
  overlayHudCorner: 1,
  vertexCache: true,
  vertexCacheSplit: true,
  bindChangedOnly: true,
  shaderCache: true,
  shaderAsync: false,
  keepAwake: true,
  video: true,
  frameRateCap: "original",
  fineLimiterSleep: true,
  physicsFrameStep: true,
  logoSkip: "start",
  motionControls: true,
  language: "english",
  quietLog: true,
  extraEnv: "",
  jobs: 0,
  noPromote: false,
  relift: false,
  autoUpdateCheck: true,
};

/** The European release: the game's language can be chosen. */
function isEurope(): boolean {
  return status?.game.titleId === "BCES00050" || status?.builds?.region === "BCES00050";
}

function syncDependent() {
  $<HTMLInputElement>("gamma").disabled = !settings.gammaOverride;
  for (const id of ["mouse-sens", "mouse-invert", "mouse-invert-x", "mouse-dz", "mouse-capture"]) $<HTMLInputElement>(id).disabled = !settings.mouse;
  $<HTMLInputElement>("rumble-strength").disabled = !settings.rumble;
  $<HTMLInputElement>("audio-volume").disabled = settings.audioMute;
  $<HTMLInputElement>("lut-strength").disabled = !settings.lutPath;
  $<HTMLButtonElement>("lut-clear").disabled = !settings.lutPath;
  $<HTMLInputElement>("vcache-split").disabled = !settings.vertexCache;
  $<HTMLInputElement>("overlay-hud").disabled = !settings.overlay;
  $<HTMLSelectElement>("hud-corner").disabled = !settings.overlay || !settings.overlayHud;
  $<HTMLSelectElement>("language").disabled = !isEurope();
  // the game's own window: no MetalFX, no borderless, no screen choice
  for (const id of ["upscale", "q-upscale", "display"]) $<HTMLSelectElement>(id).disabled = !settings.hostWindow;
  $("language-hint").textContent = t(isEurope() ? "settings.languageHintEu" : "settings.languageHintUs");
}

function applyLang(l: Lang) {
  setLang(l);
  translateDom();
  fillFixedSelects();
  syncAll();
  renderHome();
  renderFinderIcon();
  renderProgress();
  renderPrereqs();
  renderPort();
  renderRun();
  renderDisc();
  renderControls();
  renderMods();
  fillScreens();
  fillAudioOutputs();
  renderFeatures();
  renderSaves();
  renderLog();
  renderUpdates();
}

// ---- the Dock / Finder icon from the disc ----------------------------------------
function renderFinderIcon(msg?: string) {
  const can = !!status?.appBundle && status.os === "macos" && (!!status.icon || status.finderIcon);
  show($("finder-icon-row"), can);
  $("finder-icon").textContent = t(status?.finderIcon ? "settings.finderIconUnset" : "settings.finderIconSet");
  $("finder-icon-msg").textContent = msg ?? t("settings.finderIconHint");
}

async function toggleFinderIcon() {
  if (!status) return;
  const btn = $<HTMLButtonElement>("finder-icon");
  btn.disabled = true;
  try {
    status.finderIcon = await invoke<boolean>("finder_icon", { set: !status.finderIcon });
    renderFinderIcon(t("settings.finderIconDone"));
  } catch (e) {
    renderFinderIcon(t("settings.finderIconFailed", { e: String(e) }));
  } finally {
    btn.disabled = false;
  }
}

// ---- home ---------------------------------------------------------------------
let statusSeq = 0;
async function refreshStatus() {
  const seq = ++statusSeq;
  const s = await invoke<Status>("get_status");
  if (seq !== statusSeq) return;
  status = s;
  gameRunning = s.gameRunning;
  rebuildRunning = s.rebuildRunning || rebuildRunning;
  $("app-version").textContent = `v${s.version}`;
  renderHome();
  renderFinderIcon();
  renderPort();
  renderRun();
  renderDisc();
  renderFeatures();
  syncDependent();
  refreshSteam();
}
listen("status-changed", () => refreshStatus());

/** What keeps Play from working, the first one only. */
function blocker(): { title: Key; text: string; act: Key; run: () => void; act2?: Key; run2?: () => void } | null {
  if (!status || !status.hostSupported) return null;
  if (!status.repo) {
    return {
      title: "block.noRepo",
      text: status.repoInvalid ? t("block.repoInvalid", { dir: tilde(status.repoInvalid) }) : t("block.noRepoText"),
      act: "port.pickShort",
      run: pickRepoDir,
    };
  }
  if (installCard()) return null; // the Install the game card says it all
  const g = status.game;
  if (g.envFile && g.path && !g.exists) {
    return { title: "block.gameGone", text: t("block.gameGoneText", { path: tilde(g.path) }), act: "disc.other", run: () => pickGameAndGo("iso") };
  }
  if (g.dir && g.titleId && !g.ours) {
    return { title: "block.notOurs", text: t("block.notOursText", { id: g.titleId, dir: tilde(g.dir) }), act: "disc.other", run: () => pickGameAndGo("iso") };
  }
  return null;
}

function regionName(id: string): string {
  const k = REGIONS[id];
  return k ? t(k) : "";
}

function renderHome() {
  if (!status) return;
  const g = status.game;
  const b = status.builds;
  const cover = $<HTMLImageElement>("cover");
  if (status.icon) cover.src = status.icon;
  show(cover, !!status.icon);
  $("hero-art").style.backgroundImage = status.background ? `url(${status.background})` : "";
  $("hero").classList.toggle("no-art", !status.background);
  $("game-title").textContent = g.title || "Folklore";
  const id = g.titleId || b?.region || "";
  const idParts = [id && REGIONS[id] && id !== "1.10" ? id : "", regionName(id), g.appVer ? `${t("home.version")} ${g.appVer}` : ""].filter(Boolean);
  $("game-id").textContent = idParts.length ? idParts.join(" · ") : "BCUS98147 · BCES00050";
  const meta: string[] = [];
  if (b?.builtAt) meta.push(`${t("home.builtAt")} ${new Date(b.builtAt * 1000).toLocaleString(lang())}`);
  if (status.repo) meta.push(tilde(status.repo));
  $("game-meta").textContent = meta.join(" · ");

  const block = blocker();
  show($("home-blocker"), !!block && !gameRunning);
  if (block) {
    $("home-blocker-title").textContent = t(block.title);
    $("home-blocker-text").textContent = block.text;
    $("home-blocker-act").textContent = t(block.act);
    $("home-blocker-act").onclick = block.run;
    const a2 = $("home-blocker-act2");
    show(a2, !!block.act2);
    if (block.act2) {
      a2.textContent = t(block.act2);
      a2.onclick = block.run2 ?? null;
    }
  }
  renderInstall();

  const play = $<HTMLButtonElement>("play");
  $("play-label").textContent = t(gameRunning ? "home.stop" : "home.play");
  play.classList.toggle("stop", gameRunning);
  play.disabled = !gameRunning && (rebuildRunning || !status.hostSupported || !b?.ok || status.otherGameRunning);
  show($("run-state"), gameRunning || status.otherGameRunning);
  $("run-state-text").textContent = t(gameRunning ? "home.running" : "home.otherRunning");
  show($("make-shortcut"), !!status.appBundle && status.os === "macos" && !!b?.ok);
  show($("home-unsupported"), !status.hostSupported);
  $<HTMLButtonElement>("debug-play").disabled = rebuildRunning || gameRunning || !b?.ok;
  renderSteam();
  renderStatusList();
  renderUpdates();

  const s = status.lastSave;
  const box = $("save-info");
  box.innerHTML = "";
  if (!s) {
    box.textContent = t("home.noSave");
  } else {
    box.append(el("strong", "", s.subtitle || s.title || s.name));
    if (s.detail) box.append(el("div", "small", s.detail));
    box.append(el("div", "small", s.modified ? new Date(s.modified * 1000).toLocaleString(lang()) : ""));
  }
}

function renderStatusList() {
  if (!status) return;
  const b = status.builds;
  const g = status.game;
  const rows: [Key, string, string?][] = [];
  rows.push([
    "home.st.state",
    rebuildRunning
      ? t(progress.kind === "install" ? "home.st.installing" : "home.st.rebuilding")
      : gameRunning
        ? t("home.st.running")
        : b?.ok
          ? t("home.st.ready")
          : t("home.st.notBuilt"),
    gameRunning || b?.ok ? "ok-text" : "warn-text",
  ]);
  rows.push(["home.st.repo", status.repo ? tilde(status.repo) : t("home.st.noRepo"), status.repo ? undefined : "warn-text"]);
  if (status.repo) {
    rows.push([
      "home.st.game",
      g.path ? tilde(g.path) + (g.exists ? "" : ` (${t("home.st.gone")})`) : t("home.st.noGame"),
      g.path && g.exists ? undefined : "warn-text",
    ]);
    rows.push(["home.st.disc", g.dir ? `${g.titleId} ${g.appVer}`.trim() + (g.isImage ? ` · ${t("home.st.mounted")}` : "") : t("home.st.notMounted")]);
    rows.push(["home.st.build", b?.exe ? (b.builtAt ? new Date(b.builtAt * 1000).toLocaleString(lang()) : t("home.st.yes")) : t("home.st.noBuild"), b?.exe ? undefined : "warn-text"]);
  }
  const dl = $("status-list");
  dl.replaceChildren();
  for (const [k, v, cls] of rows) {
    const dt = el("dt", "", t(k));
    const dd = el("dd", cls ?? "", v);
    dl.append(dt, dd);
  }
}

async function startGame(verbose: boolean) {
  const msg = $("home-msg");
  try {
    const r = await invoke<{ log: string; backup: string | null; backupError: string | null; verbose: boolean }>("play", { verbose });
    gameRunning = true;
    msg.textContent =
      t(r.verbose ? "home.startedVerbose" : "home.started", { log: tilde(r.log) }) +
      (r.backup ? t("home.backedUp", { id: r.backup }) : "") +
      (r.backupError ? t("home.backupFailed", { e: r.backupError }) : "");
    $("debug-msg").textContent = r.verbose ? msg.textContent : "";
  } catch (e) {
    const err = String(e);
    msg.textContent = err.startsWith("no-game:") ? t("block.gameGoneText", { path: tilde(err.slice(8)) }) : err;
  }
  await refreshStatus();
}

$("play").addEventListener("click", async () => {
  if (gameRunning) {
    await invoke("stop_game").catch((e) => ($("home-msg").textContent = String(e)));
    return;
  }
  await startGame(false);
});

$("make-shortcut").addEventListener("click", async () => {
  const btn = $<HTMLButtonElement>("make-shortcut");
  const msg = $("home-msg");
  btn.disabled = true;
  try {
    const path = await invoke<string>("create_play_shortcut");
    msg.textContent = t("home.shortcutDone", { path: tilde(path) });
  } catch (e) {
    msg.textContent = t("home.shortcutFailed", { e: String(e) });
  } finally {
    btn.disabled = false;
  }
});
$("go-saves").addEventListener("click", () => {
  showView("game");
  $("saves-section").scrollIntoView({ behavior: "smooth", block: "start" });
});
$("open-saves").addEventListener("click", async () => {
  const p = await invoke<Record<string, string>>("log_paths");
  invoke("reveal", { path: status?.lastSave?.folder ?? p.saves ?? p.data });
});
$("open-log").addEventListener("click", async () => {
  const p = await invoke<Record<string, string>>("log_paths");
  if (p.game) invoke("reveal", { path: p.game });
});

// ---- Game > your disc, the in-game menu ---------------------------------------------
function renderDisc() {
  if (!status) return;
  const g = status.game;
  $("disc-path").textContent = g.path ? tilde(g.path) : t("disc.none");
  const st = $("disc-state");
  st.className = "pill " + (!g.path ? "warn" : !g.exists ? "bad" : g.dir ? (g.ours ? "" : "bad") : "warn");
  st.textContent = !g.path
    ? t("disc.notChosen")
    : !g.exists
      ? t("disc.gone")
      : g.dir
        ? g.ours
          ? `${g.titleId} ${g.appVer}`.trim()
          : t("disc.notOurs")
        : t("disc.notMounted");
  const info: string[] = [];
  if (g.dir) info.push(t("disc.readFrom", { dir: tilde(g.dir) }));
  if (g.titleId) info.push(regionName(g.titleId));
  if (settings.gamePath && settings.gamePath !== g.path) info.push(t("disc.pending", { path: tilde(settings.gamePath) }));
  $("disc-info").textContent = info.filter(Boolean).join(" · ");
  show($("disc-mount"), !!g.path && g.exists && g.isImage && !g.dir);
  const o = status.overlayIni;
  $("overlay-ini").textContent = o ? (o.exists ? t("overlay.saved", { n: o.keys.length, keys: o.keys.join(", ") || "-" }) : t("overlay.none")) : "";
  $<HTMLButtonElement>("overlay-reset").disabled = !o?.exists;
}

$("disc-mount").addEventListener("click", async () => {
  $("disc-msg").textContent = t("disc.mounting");
  try {
    const d = await invoke<string>("mount_disc");
    $("disc-msg").textContent = t("disc.mounted", { dir: tilde(d) });
  } catch (e) {
    $("disc-msg").textContent = String(e);
  }
  await refreshStatus();
});
$("disc-other").addEventListener("click", () => pickGameAndGo("iso"));
$("overlay-reset").addEventListener("click", async () => {
  const yes = await ask(t("overlay.resetAsk"), { title: t("overlay.reset"), kind: "warning" });
  if (!yes) return;
  try {
    await invoke("reset_overlay_ini");
  } catch (e) {
    $("disc-msg").textContent = String(e);
  }
  await refreshStatus();
});

/** Choose a disc image (.iso) or a folder holding PS3_GAME for the next
 * install; true when one was chosen. */
async function pickGame(kind: "iso" | "folder"): Promise<boolean> {
  const p =
    kind === "iso"
      ? await open({ title: t("inst.pickDisc"), filters: [{ name: t("disc.imageFilter"), extensions: ["iso", "ISO"] }] })
      : await open({ title: t("inst.pickFolder"), directory: true });
  if (typeof p !== "string") return false;
  settings.gamePath = p;
  persist(() => {
    refreshStatus();
    refreshInstallFacts();
  });
  renderRun();
  renderInstall();
  return true;
}

/** Another disc for an installed game: choose it, then Advanced > Install. */
async function pickGameAndGo(kind: "iso" | "folder") {
  if (await pickGame(kind)) {
    showView("advanced", "install");
    $("install-msg").textContent = t("run.chosenHint");
  }
}

// ---- Add to Steam ----------------------------------------------------------------
interface SteamStatus {
  available: boolean;
  installed: boolean;
  running: boolean;
  users: number;
  added: number;
}
let steam: SteamStatus | null = null;
let steamBusy = false;

async function refreshSteam() {
  steam = await invoke<SteamStatus>("steam_status").catch(() => null);
  renderSteam();
}

function renderSteam() {
  const ok = !!steam?.available && steam.installed && status?.os === "macos" && !!status?.builds?.ok;
  const add = $<HTMLButtonElement>("steam-add");
  const remove = $<HTMLButtonElement>("steam-remove");
  show(add, ok && steam!.added < steam!.users);
  show(remove, ok && steam!.added > 0);
  add.disabled = remove.disabled = steamBusy;
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

async function steamClosed(msg: HTMLElement): Promise<boolean> {
  const st = await invoke<SteamStatus>("steam_status").catch(() => null);
  if (!st?.running) return true;
  const quit = await ask(t("steam.quitAsk"), { title: "Steam", kind: "warning", okLabel: t("steam.quit"), cancelLabel: t("install.cancel") });
  if (!quit) {
    msg.textContent = t("steam.running");
    return false;
  }
  msg.textContent = t("steam.quitting");
  await invoke("steam_quit").catch(() => {});
  for (let i = 0; i < 40; i++) {
    await sleep(500);
    const s = await invoke<SteamStatus>("steam_status").catch(() => null);
    if (!s?.running) {
      await sleep(1000);
      return true;
    }
  }
  msg.textContent = t("steam.quitFailed");
  return false;
}

async function steamAction(kind: "add" | "remove") {
  const msg = $("home-msg");
  if (steamBusy) return;
  if (kind === "add") {
    const yes = await ask(t("steam.confirmAdd"), { title: t("steam.add"), kind: "info", okLabel: t("steam.add"), cancelLabel: t("install.cancel") });
    if (!yes) return;
  }
  steamBusy = true;
  renderSteam();
  try {
    if (!(await steamClosed(msg))) return;
    if (kind === "add") {
      const r = await invoke<{ users: number; art: boolean }>("steam_add");
      msg.textContent = t("steam.added", { n: r.users }) + (r.art ? "" : " " + t("steam.noArt"));
    } else {
      const n = await invoke<number>("steam_remove");
      msg.textContent = t(n ? "steam.removed" : "steam.notThere");
    }
  } catch (e) {
    msg.textContent = String(e) === "steam-running" ? t("steam.running") : t("steam.failed", { e: String(e) });
  } finally {
    steamBusy = false;
    await refreshSteam();
  }
}
$("steam-add").addEventListener("click", () => steamAction("add"));
$("steam-remove").addEventListener("click", () => steamAction("remove"));

// ---- Home: Install the game ----------------------------------------------------
// One button from the disc image to Play: Apple's Command Line Tools (their
// own installer, waited for), Homebrew (its own installer: the window shows
// how), then setup_mac.sh <disc> with one progress bar. Pressing it again
// after a quit or a failure continues where it stopped (setup_mac.sh skips
// what is done).
const INSTALL_MINUTES = "10–30";
let installOutcome: "" | "done" | "failed" | "cancelled" = "";
let installFacts: Prereqs | null = null;
let cltWait = 0;
let installBusy = false;

/** The Install the game card: a repository is there but no build yet (or an
 * install from Home is running or just ended). */
function installCard(): boolean {
  if (!status || !status.hostSupported || !status.repo) return false;
  return !status.builds?.ok || (progress.kind === "install" && (rebuildRunning || installOutcome !== ""));
}

async function refreshInstallFacts() {
  if (!installCard()) return;
  installFacts = await invoke<Prereqs>("check_prereqs").catch(() => installFacts);
  renderInstall();
}

/** The disc the next install uses: the one chosen here, else the saved one. */
function nextGame(): string {
  return settings.gamePath || status?.game.path || "";
}

/** The plain-language reason an install stopped, from the stage and the log. */
function installError(): { text: string; detail: string } {
  const log = progress.log.slice(-400).join("\n");
  const detail = progress.errors[progress.errors.length - 1] ?? "";
  const stage = progress.stage()?.id;
  let key: Key = "inst.err.generic";
  if (/No space left on device/i.test(log)) key = "inst.err.disk";
  else if (/Could not resolve host|Failed to connect|Connection timed out|Network is unreachable|curl: \(\d+\)/i.test(log)) key = "inst.err.net";
  else if (/Homebrew is needed/.test(log)) key = "inst.err.brewMissing";
  else if (/not a supported Folklore EBOOT|no PS3_GAME|could not mount/.test(log)) key = "inst.err.disc";
  else if (stage) key = `inst.err.${stage}` as Key;
  return { text: t(key), detail };
}

function stageLabel(id: StepId | undefined): string {
  return id ? t(`inst.stage.${id}` as Key) : t("inst.starting");
}

function renderInstall() {
  const card = $("home-install");
  const on = installCard() && !gameRunning;
  show(card, on);
  if (!on) return;
  const live = progress.kind === "install" && rebuildRunning;
  const f = installFacts;
  const disc = nextGame();
  const pill = $("inst-pill");
  pill.className = "pill " + (installOutcome === "done" ? "" : "warm");
  show(pill, live || installOutcome === "done");
  pill.textContent = installOutcome === "done" ? t("inst.pillDone") : t("inst.pillRunning");

  $("inst-lead").textContent = live ? t("inst.leadRunning") : t("inst.lead", { min: INSTALL_MINUTES, gb: f?.needGb || 3 });
  $("inst-disc").textContent = disc ? t("inst.disc", { img: tilde(disc) }) : t("inst.noDisc");

  show($("inst-live"), live || installOutcome === "failed" || installOutcome === "cancelled");
  const st = progress.stage();
  $("inst-stage").textContent = cltWait ? t("inst.cltWaiting") : stageLabel(st?.id);
  const frac = progress.fraction();
  $("inst-bar").style.width = `${((live || installOutcome !== "" ? frac : 0) * 100).toFixed(1)}%`;
  const eta = progress.etaMinutes();
  const n = progress.steps.length;
  const i = st ? progress.steps.indexOf(st) + 1 : 0;
  $("inst-text").textContent = live
    ? `${t("inst.stepN", { i, n })} · ${Math.floor(frac * 100)}%` + (eta !== null ? ` · ${t("install.eta", { min: Math.max(1, Math.round(eta)) })}` : "")
    : "";

  const failed = installOutcome === "failed" && !live;
  show($("inst-error"), failed);
  if (failed) {
    const e = installError();
    $("inst-error-text").textContent = e.text;
    $("inst-error-detail").textContent = e.detail;
  }
  const start = $<HTMLButtonElement>("inst-start");
  const done = installOutcome === "done";
  show(start, !live && !cltWait && !done);
  start.disabled = installBusy || gameRunning || (rebuildRunning && !live);
  start.textContent = t(failed || installOutcome === "cancelled" ? "inst.retry" : "inst.start");
  for (const id of ["inst-pick", "inst-pick-folder"]) show($(id), !live && !cltWait && !done);
  $("inst-pick").textContent = t(disc ? "inst.pickOther" : "inst.pickDisc");
  show($("inst-cancel"), live || !!cltWait);
  for (const id of ["inst-copy", "inst-showlog"]) show($(id), progress.kind === "install" && progress.log.length > 0);
  show($("inst-report"), failed);
}

/** Wait for Apple's Command Line Tools installer: true once they are there. */
function waitForClt(): Promise<boolean> {
  return new Promise((resolve) => {
    const tick = async () => {
      if (!cltWait) return resolve(false); // cancelled
      const p = await invoke<Prereqs>("check_prereqs").catch(() => null);
      if (p?.clt) {
        window.clearInterval(cltWait);
        cltWait = 0;
        installFacts = p;
        renderInstall();
        resolve(true);
      }
    };
    cltWait = window.setInterval(tick, 5000);
    renderInstall();
  });
}

/** setup_mac.sh [<disc>]: the Home card's install ("install") or Advanced's
 * rebuild ("rebuild"). */
async function runSetup(kind: "install" | "rebuild") {
  const chosen = settings.gamePath && settings.gamePath !== status?.game.path ? settings.gamePath : null;
  progress.reset(kind);
  if (kind === "install") installOutcome = "";
  $("log").textContent = "";
  $("install-msg").textContent = "";
  await invoke<string>("start_setup", { req: { game: chosen, jobs: settings.jobs || null, noPromote: settings.noPromote, relift: settings.relift } });
  rebuildRunning = true;
}

function setupError(e: unknown): string {
  const err = String(e);
  if (err === "clt") return t("inst.clt");
  if (err === "brew") return t("inst.brew", { cmd: BREW_CMD });
  if (err === "no-game") return t("inst.noDisc");
  return err;
}

async function installGame() {
  if (installBusy || rebuildRunning) return;
  installBusy = true;
  const msg = $("inst-msg");
  msg.textContent = t("prereq.checking");
  renderInstall();
  try {
    let p = await invoke<Prereqs>("check_prereqs");
    installFacts = p;
    // 1. the disc
    if (!p.game) {
      if (!(await pickGame("iso"))) {
        msg.textContent = t("inst.noDisc");
        return;
      }
      await new Promise((r) => setTimeout(r, 400)); // the setting is saved
      p = await invoke<Prereqs>("check_prereqs");
      installFacts = p;
    }
    // 2. Homebrew: its own installer asks for the password, so not from here
    if (!p.brew) {
      msg.textContent = t("inst.brew", { cmd: BREW_CMD });
      return;
    }
    // 3. disk space and time
    if (p.freeGb !== null && p.freeGb < p.needGb) {
      const go = await ask(t("inst.diskLow", { free: p.freeGb, gb: p.needGb }), { title: t("inst.title"), kind: "warning", okLabel: t("inst.anyway"), cancelLabel: t("install.cancel") });
      if (!go) {
        msg.textContent = "";
        return;
      }
    } else if (installOutcome === "") {
      const go = await ask(t("inst.ask", { min: INSTALL_MINUTES, gb: p.needGb || 3 }), { title: t("inst.title"), kind: "info", okLabel: t("inst.start"), cancelLabel: t("install.cancel") });
      if (!go) {
        msg.textContent = "";
        return;
      }
    }
    // 4. Apple's Command Line Tools: their installer, waited for
    if (!p.clt) {
      msg.textContent = t("inst.clt");
      await invoke("install_clt").catch(() => undefined); // already open: keep waiting
      if (!(await waitForClt())) {
        msg.textContent = "";
        return;
      }
    }
    // 5. setup_mac.sh
    msg.textContent = "";
    await runSetup("install");
  } catch (e) {
    msg.textContent = setupError(e);
  } finally {
    installBusy = false;
    renderProgress();
    renderHome();
  }
}

$("inst-start").addEventListener("click", installGame);
$("inst-pick").addEventListener("click", () => pickGame("iso"));
$("inst-pick-folder").addEventListener("click", () => pickGame("folder"));
$("inst-cancel").addEventListener("click", async () => {
  if (cltWait) {
    window.clearInterval(cltWait);
    cltWait = 0;
    $("inst-msg").textContent = "";
    renderInstall();
    return;
  }
  const yes = await ask(t("inst.askCancel"), { title: t("install.cancel"), kind: "warning", okLabel: t("install.cancel"), cancelLabel: t("inst.keepGoing") });
  if (!yes) return;
  progress.cancelled = true;
  invoke("cancel_rebuild");
});
$("inst-copy").addEventListener("click", async () => {
  await navigator.clipboard.writeText(progress.log.join("\n"));
  $("inst-msg").textContent = t("inst.copied");
});
$("inst-showlog").addEventListener("click", async () => {
  const p = await invoke<Record<string, string>>("log_paths");
  invoke("reveal", { path: p.install });
});
$("inst-report").addEventListener("click", async () => {
  await navigator.clipboard.writeText(progress.log.slice(-300).join("\n")).catch(() => undefined);
  $("inst-msg").textContent = t("inst.reportCopied");
  invoke("open_url", { url: URL_ISSUE });
});

// ---- Advanced > Install: 1. requirements ---------------------------------------------
let prereqs: Prereqs | null = null;
let cltPoll = 0;
const ICON: Record<string, string> = { ok: "✓", warn: "!", fail: "✕", info: "i" };

function checkLabel(name: string): string {
  const k = `prereq.name.${name}` as Key;
  const v = t(k);
  return v === k ? name : v;
}

/** A check's detail line, translated (the English text when there is no key). */
function checkDetail(c: CheckItem): string {
  if (!c.key) return c.path ? "" : c.message;
  const k = `prereq.msg.${c.key}` as Key;
  const v = t(k, c.vars);
  return v === k ? c.message : v;
}

let prereqSeq = 0;
async function refreshPrereqs() {
  $("prereq-msg").textContent = t("prereq.checking");
  const seq = ++prereqSeq;
  const p = await invoke<Prereqs>("check_prereqs");
  if (seq !== prereqSeq) return;
  prereqs = p;
  $("prereq-msg").textContent = "";
  renderPrereqs();
  if (cltPoll && prereqs.clt) {
    window.clearInterval(cltPoll);
    cltPoll = 0;
  }
}

function prereqActions(c: CheckItem): HTMLButtonElement[] {
  if (c.level !== "warn" && c.level !== "fail") return [];
  const button = (label: Key, run: () => unknown) => {
    const b = el("button", "", t(label));
    b.addEventListener("click", () => run());
    return b;
  };
  switch (c.name) {
    case "xcode-clt":
      return [button("prereq.installClt", installClt)];
    case "brew":
      return [
        button("prereq.brewOpen", () => invoke("open_url", { url: BREW_URL })),
        button("prereq.brewCopy", async () => {
          await navigator.clipboard.writeText(BREW_CMD);
          $("prereq-msg").textContent = t("prereq.brewCopied");
        }),
      ];
    case "repo":
      return [button("port.pickShort", pickRepoDir)];
    case "game":
      return [button("run.pickIso", () => pickGame("iso").then(refreshPrereqs)), button("run.pickFolder", () => pickGame("folder").then(refreshPrereqs))];
    default:
      return [];
  }
}

function renderPrereqs() {
  const p = prereqs;
  const list = $("prereqs");
  list.innerHTML = "";
  const supported = status?.hostSupported ?? p?.supported ?? true;
  show($("install-unsupported"), !supported);
  for (const id of ["card-prereqs", "card-port", "card-run"]) $(id).classList.toggle("disabled", !supported);
  if (!p || !p.supported) {
    $("prereq-summary").textContent = "";
    return;
  }
  const fails = p.checks.filter((c) => c.level === "fail");
  const warns = p.checks.filter((c) => c.level === "warn");
  for (const c of [...fails, ...warns, ...p.checks.filter((c) => c.level !== "fail" && c.level !== "warn")]) {
    const li = el("li", `check check-${c.level}`);
    li.append(el("span", "check-ico", ICON[c.level] ?? "·"));
    const body = el("div", "check-body");
    const title = el("div", "check-title");
    title.append(el("b", "", checkLabel(c.name)));
    body.append(title);
    const detail = checkDetail(c);
    if (detail) body.append(el("div", "check-detail", detail));
    if (c.path) {
      const path = el("div", "check-path", tilde(c.path));
      path.title = c.path;
      body.append(path);
    }
    const acts = prereqActions(c);
    if (acts.length) {
      const row = el("div", "check-actions");
      row.append(...acts);
      body.append(row);
    }
    li.append(body);
    list.appendChild(li);
  }
  const sum = $("prereq-summary");
  sum.className = "pill " + (fails.length ? "bad" : warns.length ? "warn" : "");
  sum.textContent = fails.length
    ? t("prereq.missingN", { n: fails.length })
    : warns.length
      ? t(warns.length === 1 ? "prereq.readyWarning" : "prereq.readyWarnings", { n: warns.length })
      : t("prereq.allOk");
  $("card-prereqs").classList.toggle("done", p.ok);
}

async function installClt() {
  const yes = await ask(t("prereq.askClt"), { title: t("prereq.installClt"), kind: "info" });
  if (!yes) return;
  try {
    await invoke("install_clt");
    $("prereq-msg").textContent = t("prereq.cltStarted");
    if (!cltPoll) cltPoll = window.setInterval(refreshPrereqs, 15000);
  } catch (e) {
    $("prereq-msg").textContent = String(e);
  }
}
$("prereq-recheck").addEventListener("click", refreshPrereqs);

// ---- Advanced > Install: 2. the repository folder -----------------------------------
let found: Builds[] = [];

function buildsSummary(b: Builds): string {
  if (b.exe) return t("port.ok", { date: b.builtAt ? new Date(b.builtAt * 1000).toLocaleString(lang()) : "?" }) + (b.region ? " " + t("port.region", { r: regionName(b.region) || b.region }) : "");
  return t("port.notBuiltYet");
}

function renderPort() {
  const input = $<HTMLInputElement>("repo-dir");
  if (document.activeElement !== input) input.value = settings.repoDir;
  const b = status?.builds ?? null;
  const box = $("port-builds");
  show(box, !!status?.repo);
  const sum = $("port-summary");
  if (status?.repo && b) {
    box.replaceChildren(
      el("div", "", (settings.repoDir ? "" : t("port.autoFound") + " ") + tilde(status.repo)),
      el("div", b.exe ? "ok-text small" : "warn-text small", buildsSummary(b)),
    );
    sum.className = "pill " + (b.ok ? "" : "warn");
    sum.textContent = b.ok ? t("port.ready") : t("port.notReady");
  } else {
    sum.className = "pill warn";
    sum.textContent = status?.repoInvalid ? t("port.invalid") : t("port.none");
  }
  $("card-port").classList.toggle("done", !!b?.ok);
  const ul = $("port-list");
  ul.replaceChildren();
  for (const f of found) {
    if (status?.repo && f.root === status.repo) continue;
    const li = el("li");
    const what = el("div", "grow");
    what.append(el("div", "", tilde(f.root)), el("div", f.exe ? "ok-text small" : "small muted", buildsSummary(f)));
    const use = el("button", "", t("port.use"));
    use.addEventListener("click", () => setRepoDir(f.root));
    li.append(what, use);
    ul.append(li);
  }
}

function setRepoDir(p: string) {
  settings.repoDir = p;
  $<HTMLInputElement>("repo-dir").value = p;
  persist(async () => {
    await refreshStatus();
    refreshPrereqs();
  });
}

async function pickRepoDir() {
  showView("advanced", "install");
  const p = await open({ directory: true, title: t("port.title"), defaultPath: settings.repoDir || status?.repo || undefined });
  if (typeof p === "string") setRepoDir(p);
}

async function findRepos() {
  $("port-summary").textContent = t("port.searching");
  found = await invoke<Builds[]>("find_repos").catch(() => []);
  renderPort();
}

$("pick-repo-dir").addEventListener("click", pickRepoDir);
$("find-repos").addEventListener("click", findRepos);
$("repo-dir").addEventListener("change", () => setRepoDir($<HTMLInputElement>("repo-dir").value.trim()));

// ---- Advanced > Install: 3. setup_mac.sh ---------------------------------------------
function renderRun() {
  if (!settings) return;
  const g = status?.game;
  const chosen = settings.gamePath && settings.gamePath !== g?.path ? settings.gamePath : "";
  $("run-game").textContent = chosen
    ? t("run.gameChosen", { path: tilde(chosen) })
    : g?.path
      ? t("run.gameSaved", { path: tilde(g.path) })
      : t("run.gameNone");
  show($("run-pick-clear"), !!chosen && !!g?.path);
  const start = $<HTMLButtonElement>("start");
  start.textContent = t(status?.builds?.exe && !chosen ? "run.start" : "run.install");
  start.disabled = rebuildRunning || gameRunning || !status?.repo || (!g?.envFile && !chosen);
}

$("run-pick-iso").addEventListener("click", () => pickGame("iso"));
$("run-pick-folder").addEventListener("click", () => pickGame("folder"));
$("run-pick-clear").addEventListener("click", () => {
  settings.gamePath = "";
  persist(() => refreshStatus());
  renderRun();
});

async function startRebuild() {
  if (prereqs && !prereqs.ok && !(await ask(t("install.askMissing"), { title: t("run.start"), kind: "warning" }))) return;
  try {
    await runSetup("rebuild");
  } catch (e) {
    $("install-msg").textContent = setupError(e);
  }
  renderProgress();
  renderHome();
}
$("start").addEventListener("click", startRebuild);
$("cancel").addEventListener("click", () => {
  progress.cancelled = true;
  invoke("cancel_rebuild");
});
$("copy-log").addEventListener("click", () => navigator.clipboard.writeText(progress.log.join("\n")));
async function openLogs() {
  const p = await invoke<Record<string, string>>("log_paths");
  invoke("reveal", { path: p.logs ?? p.data });
}
$("open-logs").addEventListener("click", openLogs);

function renderProgress() {
  show($("cancel"), rebuildRunning);
  renderRun();
  const steps = $("steps");
  steps.innerHTML = "";
  for (const s of progress.steps) {
    const li = el("li", `step-${s.state}`);
    li.append(el("span", "name", t(`step.${s.id}` as Key)), el("span", "state", t(`state.${s.state}` as Key) + (s.secs ? ` · ${clock(s.secs)}` : "")));
    steps.appendChild(li);
  }
  const frac = progress.fraction();
  $("bar").style.width = `${(frac * 100).toFixed(1)}%`;
  const eta = progress.etaMinutes();
  $("progress-text").textContent = rebuildRunning
    ? `${Math.floor(frac * 100)}%` + (eta !== null ? ` · ${t("install.eta", { min: Math.max(1, Math.round(eta)) })}` : "")
    : "";
  renderInstall();
}

let frame = 0;
function flushLog() {
  frame = 0;
  const pre = $("log");
  const atBottom = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 30;
  pre.textContent = progress.log.slice(-2000).join("\n");
  if (atBottom) pre.scrollTop = pre.scrollHeight;
  renderProgress();
}
listen<string>("rebuild-line", (ev) => {
  progress.feed(ev.payload);
  if (!frame) frame = requestAnimationFrame(flushLog);
});
listen<number>("rebuild-exit", (ev) => {
  rebuildRunning = false;
  const code = ev.payload;
  const msg = $("install-msg");
  if (progress.cancelled) msg.textContent = t("install.cancelled");
  else if (code === 0) msg.textContent = t(progress.kind === "pull" ? "update.pulled" : "install.done");
  else msg.textContent = t("install.failed", { code }) + (progress.errors.length ? ` ${progress.errors[progress.errors.length - 1]}` : "");
  msg.className = code === 0 ? "ok" : "bad";
  progress.finish(code === 0);
  if (progress.kind === "install") {
    installOutcome = progress.cancelled ? "cancelled" : code === 0 ? "done" : "failed";
    $("inst-msg").textContent = installOutcome === "done" ? t("inst.done") : installOutcome === "cancelled" ? t("inst.cancelled") : "";
  }
  // setup_mac.sh saved the chosen disc in folklore_mac.env
  if (code === 0 && progress.kind !== "pull" && settings.gamePath) {
    settings.gamePath = "";
    persist();
  }
  flushLog();
  refreshStatus();
  refreshUpdates(false);
});

listen<{ code: number; message: string }>("game-exit", (ev) => {
  gameRunning = false;
  const { code, message } = ev.payload;
  $("home-msg").textContent = t("home.exited", { code }) + (message ? ` ${t("home.exitedMsg", { msg: message })}` : "");
  refreshStatus();
  if ($("tab-log").classList.contains("active")) showLog();
});

// ---- Advanced > Debug ----------------------------------------------------------------
$("debug-play").addEventListener("click", () => startGame(true));
$("debug-open-logs").addEventListener("click", openLogs);
$("debug-reports").addEventListener("click", async () => {
  const p = await invoke<Record<string, string>>("log_paths");
  invoke("reveal", { path: p.reports });
});
$("debug-issue").addEventListener("click", () => invoke("open_url", { url: URL_ISSUE }));
$("debug-zip").addEventListener("click", async () => {
  const btn = $<HTMLButtonElement>("debug-zip");
  const msg = $("debug-msg");
  btn.disabled = true;
  msg.textContent = t("debug.zipping");
  try {
    const r = await invoke<{ path: string; bytes: number; files: number }>("zip_logs", { notes: $<HTMLTextAreaElement>("debug-notes").value });
    msg.textContent = t("debug.zipped", { file: tilde(r.path), size: human(r.bytes), n: r.files });
    invoke("reveal", { path: r.path });
  } catch (e) {
    msg.textContent = String(e);
  } finally {
    btn.disabled = false;
  }
});

// ---- start ---------------------------------------------------------------------
async function main() {
  settings = await invoke<Settings>("get_settings");
  setLang(settings.uiLang as Lang);
  translateDom();
  bindSettings();
  initControls();
  initMods({
    enabled: () => settings.mods ?? [],
    setEnabled: (names) => {
      settings.mods = names;
      persist();
    },
  });
  initLog();
  initSaves({
    keep: () => settings.saveBackupsKeep ?? 10,
    setKeep: (n) => {
      settings.saveBackupsKeep = n;
      persist();
    },
  });
  initUpdates({
    auto: () => settings.autoUpdateCheck ?? true,
    busy: () => (gameRunning ? "game" : rebuildRunning ? "rebuild" : null),
    canRebuild: () => !rebuildRunning && !gameRunning && !!status?.game.envFile,
    rebuild: () => {
      showView("advanced", "install");
      startRebuild();
    },
    pull: async () => {
      showView("advanced", "install");
      progress.reset("pull");
      $("log").textContent = "";
      try {
        await invoke("pull_sources");
        rebuildRunning = true;
      } catch (e) {
        $("install-msg").textContent = String(e);
      }
      renderProgress();
    },
  });
  applyLang(settings.uiLang as Lang);
  await refreshStatus();
  void refreshInstallFacts();
  refreshShaderCache();
  void refreshUpdates(settings.autoUpdateCheck ?? true);
  if (status && !status.repo) void findRepos();
  // opened by the Play shortcut (--play): start the game as the Play button would
  const playNow = await invoke<boolean>("take_play_request").catch(() => false);
  // --view <page> on the command line
  const view = await invoke<string | null>("take_start_view").catch(() => null);
  if (playNow && status?.builds?.ok && status.hostSupported && !gameRunning && !rebuildRunning) await startGame(false);
  else if (status && !status.repo) showView("advanced", "install");
  else if (view && VIEWS.includes(view)) showView(view);
}
main();
