//! Launcher core for the Folklore (BCUS98147 / BCES00050) static
//! recompilation on macOS. It drives the repository's own scripts rather
//! than doing their work again:
//!
//! - **Install / rebuild** runs `setup_mac.sh [<disc image or folder>]` with
//!   zsh in the repository folder (Homebrew packages, the Python venv, the
//!   port's own EBOOT decrypter, the lift, the build of
//!   folklore/build-mac/folklore) and streams its "==> step" lines to the
//!   window;
//! - **Play** runs `run_mac.sh KEY=VALUE ...`, which starts the game through
//!   the window host folklore.app/Contents/MacOS/folklore-window (MetalFX,
//!   the window modes, the screen; FOLKLORE_HOST_WINDOW=0 makes the game open
//!   its own window instead): the settings become
//!   environment overrides on its command line (only the ones that differ
//!   from the runtime's defaults), plus the keys of folklore_settings.ini the
//!   launcher owns (frame-rate cap, opening logos, motion controls, physics
//!   step, limiter sleep, language).
//!
//! No game code or data is part of the app: the art it shows comes from the
//! player's own disc (mounted read-only at `<repo>/gamedisc`, as
//! mac_common.sh does), and everything it starts is in the player's
//! repository folder.

mod app_icon;
mod input_map;
mod mods;
mod play_shortcut;
mod saves;
mod steam;
mod steam_art;
mod update;
mod vdf;
mod zipfile;

use input_map::{Bindings, InputEnv};
use saves::{read_sfo, BackupMeta, Slot};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs;
use std::io::{BufRead, BufReader, Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::Mutex;
use std::thread;
use std::time::UNIX_EPOCH;
use tauri::{AppHandle, Emitter, Manager, State};

/// The game's process name (folklore/build-mac/folklore, a link into
/// folklore.app/Contents/MacOS/folklore).
const GAME_EXE: &str = "folklore";
/// The window host run_mac.sh starts the game through (ps3recomp's
/// tools/host_window, built into folklore.app as folklore-window): it owns
/// the window, MetalFX and the window modes, and closes the game when asked.
const HOST_EXE: &str = "folklore-window";
/// The free disk space the first install asks for: Homebrew's packages
/// (FFmpeg and its libraries are most of it), the venv, the lifted code
/// (~0.1 GB), the promoted copy and the build (~0.3 GB), with room to spare.
const INSTALL_NEED_GB: u64 = 3;

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

/// What the player can change. Most fields map onto one environment
/// variable run_mac.sh passes to the game (see `launch_env`); the `ini`
/// ones are keys of folklore_settings.ini (see `INI_KEYS`).
#[derive(Serialize, Deserialize, Clone, Debug)]
#[serde(rename_all = "camelCase", default)]
pub struct Settings {
    /// Launcher UI language: "pt-BR" or "en".
    pub ui_lang: String,
    /// The Folklore repository (setup_mac.sh, run_mac.sh). Empty = found on
    /// its own (the folder the launcher lives in, then the usual places).
    pub repo_dir: String,
    /// The disc image or dumped folder chosen for the next install, before
    /// setup_mac.sh has saved it in folklore_mac.env.
    pub game_path: String,
    /// Mount the disc image at <repo>/gamedisc when the launcher opens, to
    /// show its art (run_mac.sh mounts it there anyway on Play).
    pub mount_for_art: bool,
    /// PS3_WINDOW_MODE: "windowed" (the default, passes nothing),
    /// "fullscreen" (macOS's own) or "borderless" (host_display.h).
    pub window_mode: String,
    /// PS3_WINDOW_SIZE: "<w>x<h>"; empty = 1280x720.
    pub window_size: String,
    /// PS3_DISPLAY: the screen's name (as NSScreen gives it); empty = the
    /// one with the menu bar.
    pub display: String,
    /// PS3_UPSCALE: "" (a plain scale) or "metalfx" (the window host's
    /// MetalFX spatial scaler).
    pub upscale: String,
    /// Through the window host (run_mac.sh's default). Off:
    /// FOLKLORE_HOST_WINDOW=0, the game opens its own window (no MetalFX, no
    /// borderless, no screen choice), for troubleshooting.
    pub host_window: bool,
    /// PS3_VSYNC=0 when off.
    pub vsync: bool,
    /// PS3_RES_SCALE: "" (native 720p), "1.5", "2" or "3".
    pub res_scale: String,
    /// The player's own ICON0.PNG in the Dock instead of the placeholder.
    pub game_icon: bool,
    /// PS3_AA: "" (off), "fxaa" or "smaa".
    pub aa: String,
    /// PS3_SHARPEN: 0 (off) to 1.
    pub sharpen: f32,
    /// PS3_ANISO: 0 (off), 2, 4, 8 or 16.
    pub aniso: u32,
    /// PS3_DISPLAY_GAMMA when set; otherwise the game's own brightness.
    pub gamma_override: bool,
    pub gamma: f32,
    /// PS3_VIBRANCE (-1..1), PS3_CONTRAST (0.5..1.5), PS3_VIGNETTE and
    /// PS3_GRAIN (0..1): the present pass's filters.
    pub vibrance: f32,
    pub contrast: f32,
    pub vignette: f32,
    pub grain: f32,
    /// PS3_LUT (the player's colour LUT, ReShade's PNG layout) and
    /// PS3_LUT_STRENGTH.
    pub lut_path: String,
    pub lut_strength: f32,
    /// PS3_MODS: the texture packs in use (folders in <data>/mods), first
    /// wins; empty = none.
    pub mods: Vec<String>,
    /// PS3_MODS_DUMP=<data>/texture-dump.
    pub mods_dump: bool,
    /// PS3_INPUT_MODE: "both" (the default), "controller" or "keyboard".
    pub input_mode: String,
    /// PS3_MOUSE, PS3_MOUSE_SENS, PS3_MOUSE_INVERT_Y, PS3_MOUSE_INVERT_X,
    /// PS3_MOUSE_DEADZONE, PS3_MOUSE_CAPTURE.
    pub mouse: bool,
    pub mouse_sens: f32,
    pub mouse_invert_y: bool,
    pub mouse_invert_x: bool,
    pub mouse_deadzone: u32,
    pub mouse_capture: bool,
    /// PS3_RUMBLE=0 when off; PS3_RUMBLE_STRENGTH in percent (0-200).
    pub rumble: bool,
    pub rumble_strength: u32,
    /// PS3_AUDIO_VOLUME (0-100), PS3_AUDIO_MUTE=1, PS3_AUDIO_DEVICE.
    pub audio_volume: u32,
    pub audio_mute: bool,
    pub audio_device: String,
    /// PS3RECOMP_METAL_FPS=1: frame rate in the window title and the log.
    pub show_fps: bool,
    /// PS3_OVERLAY=0 when off: the in-game menu (F1, Cmd+, or L3+R3).
    pub overlay: bool,
    /// PS3_OVERLAY_HUD=1: the in-game frame-time HUD; PS3_OVERLAY_HUD_CORNER
    /// 0-3 (top left, top right (the default), bottom left, bottom right).
    pub overlay_hud: bool,
    pub overlay_hud_corner: u32,
    /// Which one-off changes of defaults this settings file has had (1: the
    /// frame-rate title and the HUD turned on, 2026-11-10).
    #[serde(default)]
    pub defaults_rev: u32,
    /// PS3RECOMP_ENG_VCACHE=0 / PS3RECOMP_ENG_VCACHE_SPLIT=0 when off.
    pub vertex_cache: bool,
    pub vertex_cache_split: bool,
    /// PS3RECOMP_METAL_BIND_ALL=1 when off.
    pub bind_changed_only: bool,
    /// Off: PS3_SHADER_CACHE= (empty), run_mac.sh's folklore/build-mac/
    /// shader_cache unused.
    pub shader_cache: bool,
    /// PS3_SHADER_ASYNC=1 (opt-in).
    pub shader_async: bool,
    /// Run under `caffeinate -i` so the Mac does not sleep mid-game.
    pub keep_awake: bool,
    /// PS3RECOMP_FFMPEG=0 when off (no movies).
    pub video: bool,
    /// folklore_settings.ini: frame_rate_cap ("original", "60", "unlimited").
    pub frame_rate_cap: String,
    /// folklore_settings.ini: fine_limiter_sleep (1 ms limiter wake-ups).
    pub fine_limiter_sleep: bool,
    /// folklore_settings.ini: physics_frame_step (cloth and hair above 30 fps).
    pub physics_frame_step: bool,
    /// folklore_settings.ini: skip_logos ("off", "start", "always").
    pub logo_skip: String,
    /// folklore_settings.ini: motion_controls (on / off).
    pub motion_controls: bool,
    /// folklore_settings.ini: language (the European release only):
    /// english, french, spanish, german, italian.
    pub language: String,
    /// Off: PS3_VERBOSE=1 (run_mac.sh's default is 0).
    pub quiet_log: bool,
    /// Extra KEY=VALUE lines, one per line; '#' starts a comment.
    pub extra_env: String,
    /// setup_mac.sh's JOBS (0 = its default, 4), NO_PROMOTE=1, RELIFT=1.
    pub jobs: u32,
    pub no_promote: bool,
    pub relift: bool,
    /// Automatic backups of the saves made before each "Play" that are kept.
    pub save_backups_keep: u32,
    /// Look for new repository sources (git fetch) on start.
    pub auto_update_check: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Settings {
            ui_lang: "pt-BR".into(),
            repo_dir: String::new(),
            game_path: String::new(),
            mount_for_art: true,
            window_mode: "windowed".into(),
            window_size: String::new(),
            display: String::new(),
            upscale: String::new(),
            host_window: true,
            vsync: true,
            res_scale: String::new(),
            game_icon: false,
            aa: String::new(),
            sharpen: 0.0,
            aniso: 0,
            gamma_override: false,
            gamma: 1.0,
            vibrance: 0.0,
            contrast: 1.0,
            vignette: 0.0,
            grain: 0.0,
            lut_path: String::new(),
            lut_strength: 1.0,
            mods: Vec::new(),
            mods_dump: false,
            input_mode: "both".into(),
            mouse: true,
            mouse_sens: 1.0,
            mouse_invert_y: false,
            mouse_invert_x: false,
            mouse_deadzone: 30,
            mouse_capture: true,
            rumble: true,
            rumble_strength: 100,
            audio_volume: 100,
            audio_mute: false,
            audio_device: String::new(),
            show_fps: true,
            overlay: true,
            overlay_hud: true,
            overlay_hud_corner: 1,
            defaults_rev: 0,
            vertex_cache: true,
            vertex_cache_split: true,
            bind_changed_only: true,
            shader_cache: true,
            shader_async: false,
            keep_awake: true,
            video: true,
            frame_rate_cap: "original".into(),
            fine_limiter_sleep: true,
            physics_frame_step: true,
            logo_skip: "start".into(),
            motion_controls: true,
            language: "english".into(),
            quiet_log: true,
            extra_env: String::new(),
            jobs: 0,
            no_promote: false,
            relift: false,
            save_backups_keep: 10,
            auto_update_check: true,
        }
    }
}

fn home() -> PathBuf {
    std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("."))
}

/// The launcher's own folder: launcher.json, input.toml, texture packs,
/// save backups, log archives.
fn data_root() -> PathBuf {
    if cfg!(target_os = "macos") {
        home().join("Library/Application Support/folklore-recomp")
    } else {
        std::env::var_os("XDG_DATA_HOME").map(PathBuf::from).unwrap_or_else(|| home().join(".local/share")).join("folklore-recomp")
    }
}

fn settings_path() -> PathBuf {
    data_root().join("launcher.json")
}

/// The last install's or rebuild's output, as the window showed it.
fn install_log_path() -> PathBuf {
    data_root().join("install.log")
}

fn reports_dir() -> PathBuf {
    data_root().join("reports")
}

/// The controls file (PS3_INPUT_MAP). It exists once the Controls page has
/// changed a binding; until then the game's built-in layout applies.
fn input_map_path() -> PathBuf {
    data_root().join("input.toml")
}

/// Where texture packs live (PS3_MODS_DIR).
fn mods_dir() -> PathBuf {
    data_root().join("mods")
}

/// Where "Dump textures" makes the game write them (PS3_MODS_DUMP).
fn mods_dump_dir() -> PathBuf {
    data_root().join("texture-dump")
}

fn input_env() -> InputEnv {
    let p = input_map_path();
    match fs::read_to_string(&p) {
        Ok(text) => InputEnv { path: Some(s(&p)), has_mouse_section: input_map::parse(&text).has_mouse_section },
        Err(_) => InputEnv::default(),
    }
}

/// launcher.json, with the keys folklore_settings.ini holds read from it.
fn load_settings_file() -> Settings {
    let mut st: Settings = fs::read_to_string(settings_path()).ok().and_then(|s| serde_json::from_str(&s).ok()).unwrap_or_default();
    // The frame rate is shown by default now (title and HUD), as in the
    // inFAMOUS 2 and Resistance launchers: turn it on once in settings saved
    // before, then leave it to the player.
    if st.defaults_rev < 1 {
        st.show_fps = true;
        st.overlay_hud = true;
        st.defaults_rev = 1;
        let _ = write_settings(&st);
    }
    if let Some(repo) = repo_root(&st) {
        if let Ok(text) = fs::read_to_string(ini_path(&repo)) {
            ini_into_settings(&ini_values(&text), &mut st);
        }
    }
    st
}

fn write_settings(st: &Settings) -> Result<(), String> {
    let p = settings_path();
    if let Some(d) = p.parent() {
        fs::create_dir_all(d).map_err(|e| e.to_string())?;
    }
    let tmp = p.with_extension("json.tmp");
    fs::write(&tmp, serde_json::to_string_pretty(st).map_err(|e| e.to_string())?).map_err(|e| e.to_string())?;
    fs::rename(&tmp, &p).map_err(|e| e.to_string())
}

fn parse_env_lines(text: &str) -> Vec<(String, String)> {
    text.lines()
        .map(str::trim)
        .filter(|l| !l.is_empty() && !l.starts_with('#'))
        .filter_map(|l| {
            let l = l.strip_prefix("export ").unwrap_or(l);
            let (k, v) = l.split_once('=')?;
            let k = k.trim();
            if k.is_empty() || !k.chars().all(|c| c.is_ascii_alphanumeric() || c == '_') || k.starts_with(|c: char| c.is_ascii_digit()) {
                return None;
            }
            Some((k.to_string(), v.trim().trim_matches('"').to_string()))
        })
        .collect()
}

fn extra_var(st: &Settings, key: &str) -> Option<String> {
    parse_env_lines(&st.extra_env).into_iter().rev().find(|(k, _)| k == key).map(|(_, v)| v)
}

fn now_secs() -> u64 {
    std::time::SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

fn mtime(p: &Path) -> u64 {
    fs::metadata(p).and_then(|m| m.modified()).ok().and_then(|t| t.duration_since(UNIX_EPOCH).ok()).map_or(0, |d| d.as_secs())
}

fn base64(data: &[u8]) -> String {
    const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut s = String::with_capacity(data.len().div_ceil(3) * 4);
    for c in data.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        s.push(T[(n >> 18) as usize & 63] as char);
        s.push(T[(n >> 12) as usize & 63] as char);
        s.push(if c.len() > 1 { T[(n >> 6) as usize & 63] as char } else { '=' });
        s.push(if c.len() > 2 { T[n as usize & 63] as char } else { '=' });
    }
    s
}

fn data_url(path: &Path, mime: &str) -> Option<String> {
    let mut buf = Vec::new();
    fs::File::open(path).ok()?.read_to_end(&mut buf).ok()?;
    Some(format!("data:{mime};base64,{}", base64(&buf)))
}

fn s(p: &Path) -> String {
    p.to_string_lossy().into_owned()
}

// ---------------------------------------------------------------------------
// folklore_settings.ini (folklore/src/engine/engine_settings.cpp)
// ---------------------------------------------------------------------------

/// The keys of folklore_settings.ini the launcher sets. Every other line of
/// the file (the Windows engine menu's graphics keys, game_dir, comments)
/// stays as it is.
const INI_KEYS: &[&str] = &["frame_rate_cap", "fine_limiter_sleep", "physics_frame_step", "skip_logos", "motion_controls", "language"];
const LANGUAGES: &[&str] = &["english", "french", "spanish", "german", "italian"];

/// run_mac.sh's FOLKLORE_SETTINGS.
fn ini_path(repo: &Path) -> PathBuf {
    repo.join("folklore_settings.ini")
}

/// key -> value, as engineSettingsLoad reads them (the last one wins).
fn ini_values(text: &str) -> BTreeMap<String, String> {
    let mut m = BTreeMap::new();
    for line in text.lines() {
        if line.starts_with('#') || line.starts_with(';') {
            continue;
        }
        if let Some((k, v)) = line.split_once('=') {
            m.insert(k.trim().to_string(), v.trim().to_string());
        }
    }
    m
}

/// engine_settings.cpp's parseBool.
fn ini_bool(v: &str) -> bool {
    matches!(v, "1" | "true" | "on" | "yes")
}

fn ini_into_settings(v: &BTreeMap<String, String>, st: &mut Settings) {
    if let Some(x) = v.get("frame_rate_cap") {
        st.frame_rate_cap = match x.as_str() {
            "60" => "60",
            "unlimited" => "unlimited",
            _ => "original",
        }
        .into();
    }
    if let Some(x) = v.get("fine_limiter_sleep") {
        st.fine_limiter_sleep = ini_bool(x);
    }
    if let Some(x) = v.get("physics_frame_step") {
        st.physics_frame_step = ini_bool(x);
    }
    if let Some(x) = v.get("skip_logos") {
        st.logo_skip = match x.as_str() {
            "always" => "always",
            "start" => "start",
            o if ini_bool(o) => "start",
            _ => "off",
        }
        .into();
    }
    if let Some(x) = v.get("motion_controls") {
        st.motion_controls = !matches!(x.as_str(), "off" | "0" | "false" | "auto_capture");
    }
    if let Some(x) = v.get("language") {
        let l = x.to_ascii_lowercase();
        st.language = if LANGUAGES.contains(&l.as_str()) { l } else { "english".into() };
    }
}

fn ini_wanted(st: &Settings) -> Vec<(&'static str, String)> {
    let cap = match st.frame_rate_cap.as_str() {
        "60" => "60",
        "unlimited" => "unlimited",
        _ => "original",
    };
    let logos = match st.logo_skip.as_str() {
        "always" => "always",
        "off" => "off",
        _ => "start",
    };
    let lang = if LANGUAGES.contains(&st.language.as_str()) { st.language.as_str() } else { "english" };
    let v: Vec<(&'static str, String)> = vec![
        ("frame_rate_cap", cap.into()),
        ("fine_limiter_sleep", if st.fine_limiter_sleep { "1" } else { "0" }.into()),
        ("physics_frame_step", if st.physics_frame_step { "1" } else { "0" }.into()),
        ("skip_logos", logos.into()),
        ("motion_controls", if st.motion_controls { "on" } else { "off" }.into()),
        ("language", lang.into()),
    ];
    debug_assert!(v.iter().map(|(k, _)| *k).eq(INI_KEYS.iter().copied()));
    v
}

/// The file's text with `want` set: a key's existing lines are rewritten in
/// place, missing keys are appended under a comment.
fn ini_apply(text: &str, want: &[(&str, String)]) -> String {
    let mut out = String::new();
    let mut done = std::collections::BTreeSet::new();
    for line in text.lines() {
        let key = (!line.starts_with('#') && !line.starts_with(';')).then(|| line.split_once('=').map(|(k, _)| k.trim())).flatten();
        match key.and_then(|k| want.iter().find(|(w, _)| *w == k)) {
            Some((k, v)) => {
                out.push_str(&format!("{k} = {v}\n"));
                done.insert(*k);
            }
            None => {
                out.push_str(line);
                out.push('\n');
            }
        }
    }
    let missing: Vec<&(&str, String)> = want.iter().filter(|(k, _)| !done.contains(k)).collect();
    if !missing.is_empty() {
        if out.is_empty() {
            out.push_str("# Folklore port settings (engine_settings.cpp reads this file)\n");
        }
        out.push_str("# Set by the Folklore Launcher\n");
        for (k, v) in missing {
            out.push_str(&format!("{k} = {v}\n"));
        }
    }
    out
}

/// Write the launcher's keys into folklore_settings.ini (only when they change).
fn write_ini(repo: &Path, st: &Settings) -> Result<(), String> {
    let p = ini_path(repo);
    let old = fs::read_to_string(&p).unwrap_or_default();
    let new = ini_apply(&old, &ini_wanted(st));
    if new == old {
        return Ok(());
    }
    let tmp = p.with_extension("ini.tmp");
    fs::write(&tmp, &new).map_err(|e| format!("{}: {e}", tmp.display()))?;
    fs::rename(&tmp, &p).map_err(|e| format!("{}: {e}", p.display()))
}

// ---------------------------------------------------------------------------
// The repository folder
// ---------------------------------------------------------------------------

/// A Folklore checkout with the macOS scripts.
fn is_repo(p: &Path) -> bool {
    p.join("setup_mac.sh").is_file() && p.join("run_mac.sh").is_file() && p.join("folklore").is_dir()
}

/// Where the repository can be, in order: FOLKLORE_ROOT; a folder above the
/// running executable (the launcher lives in <repo>/launcher, and its builds
/// in launcher/src-tauri/target); the repository the launcher was built from
/// (development builds).
fn near_candidates() -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = Vec::new();
    if let Some(r) = std::env::var_os("FOLKLORE_ROOT").filter(|r| !r.is_empty()) {
        v.push(PathBuf::from(r));
    }
    if let Ok(exe) = std::env::current_exe() {
        v.extend(exe.ancestors().skip(1).map(Path::to_path_buf));
    }
    v.push(Path::new(env!("CARGO_MANIFEST_DIR")).join("../.."));
    v
}

/// Then a folder one level under the usual places (a copy of the app that
/// lives elsewhere, e.g. in Applications).
fn scanned_candidates() -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = Vec::new();
    for base in ["Desktop", "Developer", "Documents", "Projects", "src", "code", "git", ""] {
        let Ok(it) = fs::read_dir(home().join(base)) else { continue };
        let mut found: Vec<PathBuf> =
            it.flatten().filter(|e| !e.file_name().to_string_lossy().starts_with('.')).map(|e| e.path()).filter(|p| p.join("setup_mac.sh").is_file()).collect();
        found.sort();
        v.extend(found);
    }
    v
}

/// Every repository found, without repeats.
fn find_repos_now() -> Vec<PathBuf> {
    let mut seen = std::collections::HashSet::new();
    near_candidates()
        .into_iter()
        .chain(scanned_candidates())
        .filter(|p| is_repo(p))
        .map(|p| p.canonicalize().unwrap_or(p))
        .filter(|p| seen.insert(p.clone()))
        .collect()
}

/// The repository in use: the setting when it is one, else the first found
/// (the places near the launcher before the scan).
fn repo_root(st: &Settings) -> Option<PathBuf> {
    let p = st.repo_dir.trim();
    if !p.is_empty() {
        let p = PathBuf::from(p);
        return is_repo(&p).then_some(p);
    }
    // Only the places tied to this launcher are adopted by themselves. A
    // folder the scan finds elsewhere runs its own scripts, so it is offered
    // in the list (find_repos) and used once the player picks it (repo_dir).
    near_candidates()
        .into_iter()
        .find(|p| is_repo(p))
        .map(|p| p.canonicalize().unwrap_or(p))
}

// ---------------------------------------------------------------------------
// The game: folklore_mac.env, the disc, the build
// ---------------------------------------------------------------------------

/// mac_common.sh's SETTINGS file (FOLKLORE_GAME=<.iso or folder>).
fn env_file(repo: &Path) -> PathBuf {
    repo.join("folklore_mac.env")
}

/// mac_common.sh's DISC_MOUNT.
fn disc_mount(repo: &Path) -> PathBuf {
    repo.join("gamedisc")
}

/// A zsh word as `${(q)…}` writes it (backslashes, '…', "…"); None for a
/// form this does not read ($'…').
fn zsh_unquote(w: &str) -> Option<String> {
    let mut out = String::new();
    let mut it = w.chars().peekable();
    while let Some(c) = it.next() {
        match c {
            '\\' => out.push(it.next()?),
            '\'' => loop {
                match it.next()? {
                    '\'' => break,
                    x => out.push(x),
                }
            },
            '"' => loop {
                match it.next()? {
                    '"' => break,
                    '\\' => {
                        let x = it.next()?;
                        if !matches!(x, '"' | '\\' | '$' | '`') {
                            out.push('\\');
                        }
                        out.push(x);
                    }
                    '$' | '`' => return None,
                    x => out.push(x),
                }
            },
            '$' => return None,
            x => out.push(x),
        }
    }
    Some(out)
}

/// FOLKLORE_GAME from folklore_mac.env (the file setup_mac.sh writes).
fn saved_game(repo: &Path) -> Option<String> {
    let f = env_file(repo);
    let text = fs::read_to_string(&f).ok()?;
    let line = text.lines().rev().find(|l| l.trim_start().starts_with("FOLKLORE_GAME="))?;
    let raw = line.trim_start().trim_start_matches("FOLKLORE_GAME=").trim_end();
    // Read as text only, never run: a form this parser does not know
    // (zsh's $'...') is treated as no game saved, and the player picks the
    // disc again.
    zsh_unquote(raw)
}

/// The folder holding PS3_GAME: a dumped folder as it is, or the image
/// mounted at <repo>/gamedisc.
fn game_dir(repo: &Path, game: Option<&str>) -> Option<PathBuf> {
    let g = PathBuf::from(game?);
    if g.is_dir() {
        if g.join("PS3_GAME").is_dir() {
            return Some(g);
        }
        if g.file_name().is_some_and(|n| n == "PS3_GAME") {
            return g.parent().map(Path::to_path_buf);
        }
        return None;
    }
    let m = disc_mount(repo);
    m.join("PS3_GAME").is_dir().then_some(m)
}

/// A chosen game is usable by setup_mac.sh: a disc image file, or a folder
/// holding PS3_GAME.
fn valid_game(p: &Path) -> bool {
    p.is_file() || p.join("PS3_GAME").is_dir()
}

/// mac_common.sh's mount_game for an image: attach it read-only at
/// <repo>/gamedisc (left mounted, as the scripts leave it).
fn mount_image(repo: &Path, image: &Path) -> Result<PathBuf, String> {
    let m = disc_mount(repo);
    if m.join("PS3_GAME").is_dir() {
        return Ok(m);
    }
    fs::create_dir_all(&m).map_err(|e| format!("{}: {e}", m.display()))?;
    let out = Command::new("/usr/bin/hdiutil")
        .args(["attach", "-readonly", "-nobrowse", "-noverify", "-mountpoint"])
        .arg(&m)
        .arg(image)
        .stdin(Stdio::null())
        .output()
        .map_err(|e| format!("hdiutil: {e}"))?;
    if !out.status.success() {
        return Err(format!("hdiutil attach: {}", String::from_utf8_lossy(&out.stderr).trim()));
    }
    if m.join("PS3_GAME").is_dir() {
        Ok(m)
    } else {
        Err(format!("{}: mounted, but it holds no PS3_GAME folder (not a PS3 disc image?)", image.display()))
    }
}

/// setup_mac.sh's EBOOT sizes: the release a decrypted EBOOT.elf is.
fn region_of_elf(p: &Path) -> &'static str {
    match fs::metadata(p).map(|m| m.len()).unwrap_or(0) {
        6518936 => "BCUS98147",
        6518912 => "BCES00050",
        6520504 | 6520480 => "1.10",
        _ => "",
    }
}

#[derive(Serialize, Default, Clone)]
#[serde(rename_all = "camelCase")]
pub struct GameInfo {
    /// folklore_mac.env exists (setup_mac.sh has run with a game).
    env_file: bool,
    /// FOLKLORE_GAME, the disc image or folder.
    path: Option<String>,
    exists: bool,
    is_image: bool,
    /// The folder holding PS3_GAME now (the folder itself, or gamedisc/).
    dir: Option<String>,
    title: String,
    title_id: String,
    app_ver: String,
    /// The disc is one of the two releases the Mac build runs.
    ours: bool,
}

fn game_info(repo: &Path) -> GameInfo {
    let path = saved_game(repo);
    let mut g = GameInfo { env_file: env_file(repo).is_file(), ..Default::default() };
    if let Some(p) = &path {
        let pp = Path::new(p);
        g.exists = pp.exists();
        g.is_image = pp.is_file();
    }
    if let Some(d) = game_dir(repo, path.as_deref()) {
        let v = read_sfo(&d.join("PS3_GAME/PARAM.SFO")).unwrap_or_default();
        g.title = v.get("TITLE").cloned().unwrap_or_default();
        g.title_id = v.get("TITLE_ID").cloned().unwrap_or_default();
        g.app_ver = v.get("APP_VER").or(v.get("VERSION")).cloned().unwrap_or_default();
        g.ours = saves::TITLE_IDS.contains(&g.title_id.as_str());
        g.dir = Some(s(&d));
    }
    g.path = path;
    g
}

/// The disc's ICON0.PNG, PIC1.PNG (the player's own; nothing ships).
fn disc_art(repo: Option<&Path>) -> (Option<PathBuf>, Option<PathBuf>) {
    let Some(repo) = repo else { return (None, None) };
    match game_dir(repo, saved_game(repo).as_deref()) {
        Some(d) => {
            let g = d.join("PS3_GAME");
            (Some(g.join("ICON0.PNG")).filter(|p| p.is_file()), Some(g.join("PIC1.PNG")).filter(|p| p.is_file()))
        }
        None => (None, None),
    }
}

#[derive(Serialize, Clone, Debug, Default)]
#[serde(rename_all = "camelCase")]
pub struct Builds {
    root: String,
    /// folklore/build-mac/folklore (the link into folklore.app).
    exe: Option<String>,
    /// folklore.app/Contents/MacOS/folklore-window, the window host (a
    /// build from before it has none: the game opens its own window).
    host: Option<String>,
    /// When it was built (the executable's mtime).
    built_at: u64,
    /// The game can be started: built, with a game saved in folklore_mac.env.
    ok: bool,
    venv: bool,
    unself: bool,
    /// A decrypted EBOOT.elf is in decrypted/.
    decrypted: bool,
    /// The lifted code is in folklore/src/recomp, and the release it was
    /// lifted from ("BCUS98147", "BCES00050", "1.10" or "").
    lifted: bool,
    region: String,
    missing: Vec<String>,
}

fn decrypted_elfs(repo: &Path) -> Vec<PathBuf> {
    fs::read_dir(repo.join("decrypted")).into_iter().flatten().flatten().map(|e| e.path().join("EBOOT.elf")).filter(|p| p.is_file()).collect()
}

fn resolve_builds(repo: &Path) -> Builds {
    let exe = repo.join("folklore/build-mac/folklore");
    let exe_ok = exe.is_file();
    let lifted = repo.join("folklore/src/recomp/ppu_recomp.h").is_file();
    let from = fs::read_to_string(repo.join("folklore/src/recomp/.lifted_from")).ok().map(|t| PathBuf::from(t.trim()));
    let elfs = decrypted_elfs(repo);
    let region = from.as_deref().map(region_of_elf).filter(|r| !r.is_empty()).or_else(|| elfs.first().map(|p| region_of_elf(p))).unwrap_or("");
    let env = env_file(repo).is_file();
    let mut missing = Vec::new();
    if !env {
        missing.push("folklore_mac.env".to_string());
    }
    if !exe_ok {
        missing.push("folklore/build-mac/folklore".to_string());
    }
    Builds {
        root: s(repo),
        built_at: if exe_ok { mtime(&exe) } else { 0 },
        exe: exe_ok.then(|| s(&exe)),
        host: Some(repo.join("folklore/build-mac/folklore.app/Contents/MacOS").join(HOST_EXE)).filter(|p| p.is_file()).map(|p| s(&p)),
        ok: exe_ok && env,
        venv: repo.join("tools/venv-mac/bin/python").exists(),
        unself: repo.join("tools/unself").is_file(),
        decrypted: !elfs.is_empty(),
        lifted,
        region: region.into(),
        missing,
    }
}

fn save_locations(st: &Settings, repo: Option<&Path>) -> saves::Locations {
    let mut roots = Vec::new();
    if let Some(r) = repo {
        // the toolkit's cwd-relative ./gamedata (run_mac.sh cds into the repo)
        let gamedata = match extra_var(st, "PS3_GAMEDATA_ROOT") {
            Some(v) if !v.is_empty() => {
                let p = PathBuf::from(v);
                if p.is_absolute() {
                    p
                } else {
                    r.join(p)
                }
            }
            _ => r.join("gamedata"),
        };
        roots.push(saves::Root { label: "gamedata".into(), dir: gamedata.join("dev_hdd0/home").join(saves::PORT_USER).join("savedata") });
        roots.push(saves::Root { label: "hdd0".into(), dir: r.join("hdd0/home").join(saves::PORT_USER).join("savedata") });
    }
    saves::Locations { roots, backups: data_root().join("save-backups") }
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SaveInfo {
    name: String,
    title: String,
    subtitle: String,
    detail: String,
    modified: u64,
    folder: String,
}

fn last_save(st: &Settings, repo: Option<&Path>) -> Option<SaveInfo> {
    saves::list_slots(&save_locations(st, repo), false)
        .into_iter()
        .max_by_key(|x| x.modified)
        .map(|x| SaveInfo { name: x.name, title: x.title, subtitle: x.subtitle, detail: x.detail, modified: x.modified, folder: x.path })
}

/// run_mac.sh's shader cache (PS3_SHADER_CACHE), unless turned off or moved
/// by an extra variable.
fn shader_cache_dir(st: &Settings, repo: Option<&Path>) -> Option<PathBuf> {
    match extra_var(st, "PS3_SHADER_CACHE") {
        Some(v) if v.is_empty() || v == "0" => None,
        Some(v) => Some(PathBuf::from(v)),
        None if st.shader_cache => repo.map(|r| r.join("folklore/build-mac/shader_cache")),
        None => None,
    }
}

// ---------------------------------------------------------------------------
// Processes
// ---------------------------------------------------------------------------

#[derive(Default)]
pub struct AppState {
    /// The install / rebuild / git pull this launcher runs.
    rebuild: Mutex<Option<u32>>,
    game: Mutex<Option<u32>>,
    /// The launcher's Dock icon as last set (app_icon::key; "" = the bundle's).
    dock_icon: Mutex<String>,
    play_request: Mutex<bool>,
    exit_with_game: Mutex<bool>,
    /// `--view <page>`: the page to open on.
    start_view: Mutex<Option<String>>,
    /// build_features' cache.
    features: Mutex<FeatureCache>,
    /// The disc image was mounted for its art this session (or tried).
    art_mount_tried: Mutex<bool>,
    /// The version an update put on disk this session (restart to finish).
    update_installed: Mutex<Option<String>>,
}

/// A GUI app on macOS starts with a bare PATH: Homebrew's and the usual
/// install places go in front (mac_common.sh does the same).
fn tool_path() -> std::ffi::OsString {
    let mut parts: Vec<PathBuf> = ["/opt/homebrew/bin", "/opt/homebrew/sbin", "/usr/local/bin"].map(PathBuf::from).to_vec();
    if let Some(p) = std::env::var_os("PATH") {
        parts.extend(std::env::split_paths(&p));
    }
    parts.extend(["/usr/bin", "/bin", "/usr/sbin", "/sbin"].map(PathBuf::from));
    let mut seen = std::collections::HashSet::new();
    parts.retain(|p| seen.insert(p.clone()));
    std::env::join_paths(parts).unwrap_or_else(|_| std::env::var_os("PATH").unwrap_or_default())
}

/// The game builds and runs on Apple-silicon Macs (Metal).
fn host_supported() -> bool {
    cfg!(target_os = "macos") || std::env::var_os("FOLKLORE_ANY_HOST").is_some_and(|v| !v.is_empty())
}

fn require_supported_host() -> Result<(), String> {
    if host_supported() {
        Ok(())
    } else {
        Err(format!("the game runs on macOS only; this is {}", std::env::consts::OS))
    }
}

/// Kept for the modules shared with the other ports' launchers (a console
/// window is a Windows matter; the Mac has none).
pub(crate) fn no_console(cmd: &mut Command) -> &mut Command {
    cmd
}

fn clt_installed() -> bool {
    if !cfg!(target_os = "macos") {
        return true;
    }
    Command::new("/usr/bin/xcode-select")
        .arg("-p")
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .output()
        .map(|o| o.status.success() && Path::new(String::from_utf8_lossy(&o.stdout).trim()).is_dir())
        .unwrap_or(false)
}

fn find_in_path(name: &str) -> Option<PathBuf> {
    std::env::split_paths(&tool_path()).map(|d| d.join(name)).find(|p| p.is_file())
}

fn find_brew() -> Option<PathBuf> {
    ["/opt/homebrew/bin/brew", "/usr/local/bin/brew"].iter().map(PathBuf::from).find(|p| p.is_file())
}

fn brew_prefix() -> PathBuf {
    match find_brew() {
        Some(b) => b.parent().and_then(Path::parent).map(Path::to_path_buf).unwrap_or_else(|| PathBuf::from("/opt/homebrew")),
        None => PathBuf::from("/opt/homebrew"),
    }
}

#[cfg(unix)]
fn own_process_group(cmd: &mut Command) {
    use std::os::unix::process::CommandExt;
    cmd.process_group(0); // so a stop reaches what it started too
}
#[cfg(not(unix))]
fn own_process_group(_cmd: &mut Command) {}

/// Stop what this launcher started, with what it started in turn: SIGTERM
/// (SIGKILL when `force`) to the process group `pid` leads.
fn kill_tree(pid: u32, force: bool) {
    let _ = Command::new("/bin/kill").arg(if force { "-KILL" } else { "-TERM" }).arg(format!("-{pid}")).status();
}

fn command_stdout(cmd: &mut Command) -> Option<String> {
    let out = cmd.stderr(Stdio::null()).output().ok()?;
    if !out.status.success() {
        return None;
    }
    String::from_utf8(out.stdout).ok()
}

/// A process with this exact name is running (the game started from a
/// terminal). Only looked at, never signalled.
fn process_running(name: &str) -> bool {
    Command::new("/usr/bin/pgrep").args(["-x", name]).stdout(Stdio::null()).stderr(Stdio::null()).status().is_ok_and(|s| s.success())
}

fn shell_quote(v: &str) -> String {
    if !v.is_empty() && v.chars().all(|c| c.is_ascii_alphanumeric() || "-_./:,=+@".contains(c)) {
        v.to_string()
    } else {
        format!("'{}'", v.replace('\'', "'\\''"))
    }
}

// ---------------------------------------------------------------------------
// Launch
// ---------------------------------------------------------------------------

/// "<w>x<h>" as PS3_WINDOW_SIZE takes it (host_display.h: 160x90 ..
/// 8192x8192; PS3RECOMP_METAL_WINDOW with the host off takes the same).
fn valid_window_size(v: &str) -> bool {
    let mut it = v.splitn(2, 'x').map(|n| n.parse::<u32>().ok());
    matches!((it.next(), it.next()), (Some(Some(w)), Some(Some(h))) if (320..=8192).contains(&w) && (180..=8192).contains(&h))
}

/// PS3_RES_SCALE for the `res_scale` setting: a multiple above 1 and up to 4
/// (the runtime's range, in quarter steps), or None for native.
fn res_scale_value(v: &str) -> Option<String> {
    let x: f32 = v.trim().parse().ok()?;
    (x > 1.0 && x <= 4.0).then(|| format!("{}", (x * 4.0).round() / 4.0))
}

/// The environment overrides Play passes to run_mac.sh (`./run_mac.sh
/// KEY=VALUE ...`): every setting that differs from the runtime's default,
/// then the extra variables (they win). run_mac.sh sets the rest itself
/// (the disc, hdd0/hdd1, folklore_settings.ini, overlay.ini, the shader and
/// MSL caches, the port's fixed switches, Homebrew's FFmpeg).
fn launch_env(st: &Settings, input: &InputEnv) -> Vec<(String, String)> {
    let mut env: Vec<(String, String)> = Vec::new();
    let mut set = |k: &str, v: String| env.push((k.to_string(), v));
    // the window: the window host's (host_display.h), or with the host off
    // (FOLKLORE_HOST_WINDOW=0) the Metal backend's own, which has only
    // macOS fullscreen and a start size
    let fullscreen = matches!(st.window_mode.as_str(), "fullscreen" | "borderless");
    if st.host_window {
        if fullscreen {
            set("PS3_WINDOW_MODE", st.window_mode.clone());
        }
        if valid_window_size(&st.window_size) {
            set("PS3_WINDOW_SIZE", st.window_size.clone());
        }
        let display = st.display.trim();
        if !display.is_empty() && !display.contains(['\n', '\r']) {
            set("PS3_DISPLAY", display.to_string());
        }
        if st.upscale == "metalfx" {
            set("PS3_UPSCALE", "metalfx".into());
        }
    } else {
        set("FOLKLORE_HOST_WINDOW", "0".into());
        if fullscreen {
            set("PS3RECOMP_FULLSCREEN", "1".into());
        }
        if valid_window_size(&st.window_size) {
            set("PS3RECOMP_METAL_WINDOW", st.window_size.clone());
        }
    }
    if !st.vsync {
        set("PS3_VSYNC", "0".into());
    }
    if let Some(k) = res_scale_value(&st.res_scale) {
        set("PS3_RES_SCALE", k);
    }
    if st.aa == "fxaa" || st.aa == "smaa" {
        set("PS3_AA", st.aa.clone());
    }
    if st.sharpen >= 0.005 {
        set("PS3_SHARPEN", format!("{:.2}", st.sharpen.min(1.0)));
    }
    if st.aniso > 1 {
        set("PS3_ANISO", st.aniso.min(16).to_string());
    }
    if st.gamma_override {
        set("PS3_DISPLAY_GAMMA", format!("{:.2}", st.gamma.clamp(0.5, 1.5)));
    }
    if st.vibrance.abs() >= 0.005 {
        set("PS3_VIBRANCE", format!("{:.2}", st.vibrance.clamp(-1.0, 1.0)));
    }
    if (st.contrast - 1.0).abs() >= 0.005 && st.contrast > 0.0 {
        set("PS3_CONTRAST", format!("{:.2}", st.contrast.clamp(0.5, 1.5)));
    }
    if st.vignette >= 0.005 {
        set("PS3_VIGNETTE", format!("{:.2}", st.vignette.min(1.0)));
    }
    if st.grain >= 0.005 {
        set("PS3_GRAIN", format!("{:.2}", st.grain.min(1.0)));
    }
    let lut = st.lut_path.trim();
    if !lut.is_empty() {
        set("PS3_LUT", lut.to_string());
        if st.lut_strength < 0.995 {
            set("PS3_LUT_STRENGTH", format!("{:.2}", st.lut_strength.clamp(0.0, 1.0)));
        }
    }
    if let Some(list) = mods::env_list(&st.mods) {
        set("PS3_MODS_DIR", s(&mods_dir()));
        set("PS3_MODS", list);
    }
    if st.mods_dump {
        set("PS3_MODS_DUMP", s(&mods_dump_dir()));
    }
    // input: the controls file, then the mouse and rumble settings (a
    // hand-written [mouse] section makes every mouse setting go, so the
    // launcher's stand)
    if let Some(p) = &input.path {
        set("PS3_INPUT_MAP", p.clone());
    }
    if matches!(st.input_mode.as_str(), "controller" | "keyboard") {
        set("PS3_INPUT_MODE", st.input_mode.clone());
    }
    let all = input.has_mouse_section;
    if !st.mouse {
        set("PS3_MOUSE", "0".into());
    } else {
        if all {
            set("PS3_MOUSE", "1".into());
        }
        if all || (st.mouse_sens - 1.0).abs() > 1e-3 {
            set("PS3_MOUSE_SENS", format!("{:.2}", st.mouse_sens.clamp(0.05, 10.0)));
        }
        if all || st.mouse_invert_y {
            set("PS3_MOUSE_INVERT_Y", if st.mouse_invert_y { "1" } else { "0" }.into());
        }
        if all || st.mouse_invert_x {
            set("PS3_MOUSE_INVERT_X", if st.mouse_invert_x { "1" } else { "0" }.into());
        }
        if all || st.mouse_deadzone != 30 {
            set("PS3_MOUSE_DEADZONE", st.mouse_deadzone.min(120).to_string());
        }
        if all || !st.mouse_capture {
            set("PS3_MOUSE_CAPTURE", if st.mouse_capture { "1" } else { "0" }.into());
        }
    }
    if !st.rumble {
        set("PS3_RUMBLE", "0".into());
    } else if st.rumble_strength != 100 {
        set("PS3_RUMBLE_STRENGTH", st.rumble_strength.min(200).to_string());
    }
    // audio: the runtime mutes on any PS3_AUDIO_MUTE value
    if st.audio_mute {
        set("PS3_AUDIO_MUTE", "1".into());
    } else if st.audio_volume < 100 {
        set("PS3_AUDIO_VOLUME", st.audio_volume.to_string());
    }
    let device = st.audio_device.trim();
    if !device.is_empty() && !device.contains(['\n', '\r']) {
        set("PS3_AUDIO_DEVICE", device.to_string());
    }
    // the in-game menu and its HUD (ps3_overlay.mm)
    if !st.overlay {
        set("PS3_OVERLAY", "0".into());
    } else if !st.overlay_hud {
        // On is the default (run_mac.sh seeds overlay.ini, so the menu can
        // still turn it off); off has to be said.
        set("PS3_OVERLAY_HUD", "0".into());
    } else if st.overlay_hud_corner != 1 {
        set("PS3_OVERLAY_HUD_CORNER", st.overlay_hud_corner.min(3).to_string());
    }
    if !st.show_fps {
        set("PS3RECOMP_METAL_FPS", "0".into()); // run_mac.sh's default is 1
    }
    if !st.vertex_cache {
        set("PS3RECOMP_ENG_VCACHE", "0".into());
    } else if !st.vertex_cache_split {
        set("PS3RECOMP_ENG_VCACHE_SPLIT", "0".into());
    }
    if !st.bind_changed_only {
        set("PS3RECOMP_METAL_BIND_ALL", "1".into());
    }
    if !st.shader_cache {
        set("PS3_SHADER_CACHE", String::new());
    }
    if st.shader_async {
        set("PS3_SHADER_ASYNC", "1".into());
    }
    if !st.video {
        set("PS3RECOMP_FFMPEG", "0".into());
    }
    if !st.quiet_log {
        set("PS3_VERBOSE", "1".into());
    }
    env.extend(parse_env_lines(&st.extra_env));
    env
}

/// The program and its arguments: [caffeinate -i] zsh run_mac.sh KEY=VALUE…
fn launch_argv(st: &Settings, repo: &Path, env: &[(String, String)]) -> Vec<String> {
    let mut argv = Vec::new();
    if st.keep_awake && Path::new("/usr/bin/caffeinate").is_file() {
        argv.extend(["/usr/bin/caffeinate".to_string(), "-i".to_string()]);
    }
    argv.push("/bin/zsh".into());
    argv.push(s(&repo.join("run_mac.sh")));
    argv.extend(env.iter().map(|(k, v)| format!("{k}={v}")));
    argv
}

/// The equivalent command line, shown in Advanced > Environment.
#[tauri::command]
fn launch_preview(settings: Settings) -> Result<String, String> {
    let repo = repo_root(&settings).ok_or("no Folklore folder yet (Advanced > Install / rebuild)")?;
    let env = launch_env(&settings, &input_env());
    let mut out = vec![format!("cd {} &&", shell_quote(&s(&repo)))];
    if settings.keep_awake {
        out.push("caffeinate -i".into());
    }
    out.push("./run_mac.sh".into());
    out.extend(env.into_iter().map(|(k, v)| format!("{k}={}", shell_quote(&v))));
    Ok(out.join(" "))
}

/// Where Play writes run_mac.sh's own output (its "==>" line and errors such
/// as "not built yet"); the game's log is logs/run-mac.log.
fn play_log_path(repo: &Path) -> PathBuf {
    repo.join("logs/launcher-play.log")
}

fn game_log_path(repo: &Path) -> PathBuf {
    repo.join("logs/run-mac.log")
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PlayStarted {
    log: String,
    backup: Option<String>,
    backup_error: Option<String>,
    verbose: bool,
}

#[derive(Serialize, Clone)]
#[serde(rename_all = "camelCase")]
pub struct GameExit {
    code: i32,
    /// The end of run_mac.sh's own output when it failed (e.g. "error: not
    /// built yet").
    message: String,
}

/// The last `n` lines of a text file.
fn tail_lines(p: &Path, n: usize) -> String {
    let text = fs::read_to_string(p).unwrap_or_default();
    let lines: Vec<&str> = text.lines().collect();
    lines[lines.len().saturating_sub(n)..].join("\n")
}

/// Start the game. `verbose`: the runtime's full log (PS3_VERBOSE=1) for
/// this run, whatever the settings say.
#[tauri::command]
fn play(app: AppHandle, state: State<'_, AppState>, verbose: Option<bool>) -> Result<PlayStarted, String> {
    require_supported_host()?;
    let mut slot = state.game.lock().unwrap();
    if slot.is_some() {
        return Err("the game is already running".into());
    }
    if state.rebuild.lock().unwrap().is_some() {
        return Err("an install or a rebuild is running: wait for it to finish".into());
    }
    let st = load_settings_file();
    let repo = repo_root(&st).ok_or("no Folklore folder: pick it in Advanced > Install / rebuild")?;
    let b = resolve_builds(&repo);
    if !b.ok {
        return Err(format!("missing in {}: {} (Install the game first)", b.root, b.missing.join(", ")));
    }
    if let Some(g) = saved_game(&repo) {
        if !Path::new(&g).exists() {
            return Err(format!("no-game:{g}"));
        }
    }
    write_ini(&repo, &st)?;
    let (backup, backup_error) = match saves::backup_before_play(&save_locations(&st, Some(&repo)), st.save_backups_keep, now_secs()) {
        Ok(b) => (b.map(|b| b.id), None),
        Err(e) => (None, Some(e)),
    };
    let verbose = verbose.unwrap_or(false);
    let mut env = launch_env(&st, &input_env());
    if verbose {
        env.push(("PS3_VERBOSE".into(), "1".into()));
    }
    let argv = launch_argv(&st, &repo, &env);
    let logs = repo.join("logs");
    fs::create_dir_all(&logs).map_err(|e| format!("{}: {e}", logs.display()))?;
    let out_path = play_log_path(&repo);
    let out = fs::File::create(&out_path).map_err(|e| format!("{}: {e}", out_path.display()))?;
    let err = out.try_clone().map_err(|e| e.to_string())?;
    let mut cmd = Command::new(&argv[0]);
    cmd.args(&argv[1..])
        .current_dir(&repo)
        .env("PATH", tool_path())
        .env("TERM", "dumb")
        .env_remove("PS3RECOMP_METAL_HEADLESS")
        .env_remove("PS3RECOMP_HOST_SURFACES")
        .env_remove("PS3RECOMP_WINDOW_STATE")
        .env_remove("FOLKLORE_EXE")
        .stdin(Stdio::null())
        .stdout(out)
        .stderr(err);
    own_process_group(&mut cmd);
    let mut child = cmd.spawn().map_err(|e| format!("{}: {e}", argv[0]))?;
    *slot = Some(child.id());
    drop(slot);
    let a = app.clone();
    thread::spawn(move || {
        let status = child.wait();
        let code = status.as_ref().ok().and_then(|s| s.code()).unwrap_or(-1);
        let st = a.state::<AppState>();
        *st.game.lock().unwrap() = None;
        let message = if code == 0 { String::new() } else { tail_lines(&out_path, 12) };
        let _ = a.emit("game-exit", GameExit { code, message });
        if code == 0 && *st.exit_with_game.lock().unwrap() {
            a.exit(0);
        }
    });
    Ok(PlayStarted { log: s(&game_log_path(&repo)), backup, backup_error, verbose })
}

/// The window hosts in the process group Play started (`pid` leads it).
fn hosts_in_group(pid: u32) -> Vec<u32> {
    command_stdout(Command::new("/usr/bin/pgrep").args(["-g", &pid.to_string(), "-x", HOST_EXE]))
        .map(|o| o.lines().filter_map(|l| l.trim().parse().ok()).collect())
        .unwrap_or_default()
}

/// Stop. Through the window host (run_mac.sh's default): SIGTERM to the
/// folklore-window Play started, which closes the game itself (SIGTERM to
/// it, SIGKILL 3 s later, as when its window is closed) and exits, and then
/// run_mac.sh ends. Without a host (FOLKLORE_HOST_WINDOW=0, or a build from
/// before it): SIGTERM to the whole group (zsh, run_mac.sh, the game). Either
/// way SIGKILL to the group 8 s later if anything of it is still there. Only
/// this launcher's own process group, never a process found by name
/// elsewhere.
#[tauri::command]
fn stop_game(app: AppHandle, state: State<'_, AppState>) {
    if let Some(pid) = *state.game.lock().unwrap() {
        let hosts = hosts_in_group(pid);
        if hosts.is_empty() {
            kill_tree(pid, false);
        } else {
            for h in hosts {
                let _ = Command::new("/bin/kill").arg("-TERM").arg(h.to_string()).status();
            }
        }
        thread::spawn(move || {
            thread::sleep(std::time::Duration::from_secs(8));
            if *app.state::<AppState>().game.lock().unwrap() == Some(pid) {
                kill_tree(pid, true);
            }
        });
    }
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct OverlayIni {
    path: String,
    exists: bool,
    /// The settings the in-game menu saved there.
    keys: Vec<String>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Status {
    /// The repository folder in use.
    repo: Option<String>,
    /// The folder set in the settings when it is not a Folklore repository.
    repo_invalid: Option<String>,
    builds: Option<Builds>,
    game: GameInfo,
    icon: Option<String>,
    background: Option<String>,
    last_save: Option<SaveInfo>,
    game_running: bool,
    /// A `folklore` this launcher did not start (run_mac.sh in a terminal).
    other_game_running: bool,
    rebuild_running: bool,
    overlay_ini: Option<OverlayIni>,
    data_root: String,
    host_supported: bool,
    os: String,
    app_bundle: Option<String>,
    finder_icon: bool,
    version: String,
    /// FEATURE_MARKERS found in the game's executable.
    features: Vec<String>,
}

/// Mount the saved disc image at <repo>/gamedisc once per session, in the
/// background, so Home can show its art; "status-changed" when done.
fn mount_for_art(app: &AppHandle, st: &Settings, repo: &Path) {
    if !st.mount_for_art || !cfg!(target_os = "macos") || disc_mount(repo).join("PS3_GAME").is_dir() {
        return;
    }
    let Some(g) = saved_game(repo).map(PathBuf::from).filter(|g| g.is_file()) else { return };
    {
        let state = app.state::<AppState>();
        let mut tried = state.art_mount_tried.lock().unwrap();
        if *tried {
            return;
        }
        *tried = true;
    }
    let (a, r) = (app.clone(), repo.to_path_buf());
    thread::spawn(move || {
        if mount_image(&r, &g).is_ok() {
            sync_app_icon(&a);
            let _ = a.emit("status-changed", ());
        }
    });
}

#[tauri::command]
async fn get_status(app: AppHandle, state: State<'_, AppState>) -> Result<Status, String> {
    let st = load_settings_file();
    let game_running = state.game.lock().unwrap().is_some();
    let rebuild_running = state.rebuild.lock().unwrap().is_some();
    let app2 = app.clone();
    tauri::async_runtime::spawn_blocking(move || {
        let repo = repo_root(&st);
        if let Some(r) = &repo {
            mount_for_art(&app2, &st, r);
        }
        let builds = repo.as_deref().map(resolve_builds);
        let features = builds.as_ref().map(|b| build_features(b, &app2.state::<AppState>().features)).unwrap_or_default();
        let (icon, pic1) = disc_art(repo.as_deref());
        sync_app_icon(&app2);
        let bundle = app_icon::app_bundle().filter(|_| cfg!(target_os = "macos"));
        let overlay_ini = repo.as_deref().map(|r| {
            let p = r.join("overlay.ini");
            let text = fs::read_to_string(&p).ok();
            OverlayIni { exists: text.is_some(), keys: text.map(|t| ini_values(&t).into_keys().filter(|k| k.starts_with("PS3")).collect()).unwrap_or_default(), path: s(&p) }
        });
        let repo_invalid = (!st.repo_dir.trim().is_empty() && repo.is_none()).then(|| st.repo_dir.trim().to_string());
        Status {
            game: repo.as_deref().map(game_info).unwrap_or_default(),
            last_save: last_save(&st, repo.as_deref()),
            repo: repo.as_deref().map(s),
            repo_invalid,
            builds,
            icon: icon.and_then(|p| data_url(&p, "image/png")),
            background: pic1.and_then(|p| data_url(&p, "image/png")),
            game_running,
            other_game_running: !game_running && (process_running(GAME_EXE) || process_running(HOST_EXE)),
            rebuild_running,
            overlay_ini,
            data_root: s(&data_root()),
            host_supported: host_supported(),
            os: std::env::consts::OS.into(),
            finder_icon: bundle.as_deref().is_some_and(app_icon::has_finder_icon),
            app_bundle: bundle.map(|b| s(&b)),
            version: env!("CARGO_PKG_VERSION").into(),
            features,
        }
    })
    .await
    .map_err(|e| e.to_string())
}

// ---------------------------------------------------------------------------
// What the build supports
// ---------------------------------------------------------------------------

/// Names whose presence in the game's executable says that it has a feature
/// the settings drive. A build made before the feature landed lacks the
/// name; the window then labels the setting ("rebuild to use it") rather
/// than pass a variable nothing reads.
const FEATURE_MARKERS: &[&str] = &[
    "PS3_INPUT_MAP",          // keyboard/mouse bindings, mouse camera, rumble
    "PS3_LUT_STRENGTH",       // SMAA and the present pass's filters
    "PS3_RES_SCALE",          // internal resolution
    "PS3_MODS_DIR",           // texture packs
    "PS3_AUDIO_VOLUME",       // volume, mute and output device
    "PS3_OVERLAY_INI",        // the in-game menu and its HUD
    "frame_rate_cap",         // folklore_settings.ini's limiter keys
];

/// The markers found in `data` (a NUL-terminated C string in the binary).
fn markers_in(data: &[u8], found: &mut std::collections::BTreeSet<String>) {
    for m in FEATURE_MARKERS {
        let mut pat = m.as_bytes().to_vec();
        pat.push(0);
        if memchr::memmem::find(data, &pat).is_some() {
            found.insert((*m).to_string());
        }
    }
}

type FeatureCache = BTreeMap<String, (u64, u64, Vec<String>)>;

/// The markers in the game's executable, cached by its size and mtime.
fn build_features(b: &Builds, cache: &Mutex<FeatureCache>) -> Vec<String> {
    let Some(p) = &b.exe else { return Vec::new() };
    let with_host = |mut v: Vec<String>| {
        // the window host's options (PS3_WINDOW_MODE, PS3_UPSCALE, ...)
        if b.host.is_some() {
            v.push("HOST_WINDOW".into());
        }
        v
    };
    let meta = fs::metadata(p).ok();
    let key = (meta.as_ref().map_or(0, |m| m.len()), b.built_at);
    if let Some(c) = cache.lock().unwrap().get(p).filter(|c| (c.0, c.1) == key) {
        return with_host(c.2.clone());
    }
    let mut set = std::collections::BTreeSet::new();
    if let Ok(data) = fs::read(p) {
        markers_in(&data, &mut set);
    }
    let v: Vec<String> = set.into_iter().collect();
    cache.lock().unwrap().insert(p.clone(), (key.0, key.1, v.clone()));
    with_host(v)
}

// ---------------------------------------------------------------------------
// Controls, texture packs, settings
// ---------------------------------------------------------------------------

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct InputMapView {
    path: String,
    exists: bool,
    bindings: Bindings,
}

#[tauri::command]
fn get_input_map() -> InputMapView {
    let p = input_map_path();
    let text = fs::read_to_string(&p);
    InputMapView { path: s(&p), exists: text.is_ok(), bindings: text.map(|t| input_map::parse(&t).bindings).unwrap_or_default() }
}

/// Write the bindings into `path`; the file's other sections stay.
fn write_input_map(path: &Path, bindings: Bindings) -> Result<(), String> {
    input_map::validate(&bindings)?;
    let mut file = fs::read_to_string(path).map(|t| input_map::parse(&t)).unwrap_or_default();
    file.bindings = bindings;
    if let Some(d) = path.parent() {
        fs::create_dir_all(d).map_err(|e| e.to_string())?;
    }
    let tmp = path.with_extension("toml.tmp");
    fs::write(&tmp, input_map::render(&file)).map_err(|e| e.to_string())?;
    fs::rename(&tmp, path).map_err(|e| e.to_string())
}

#[tauri::command]
fn save_input_map(bindings: Bindings) -> Result<String, String> {
    let p = input_map_path();
    write_input_map(&p, bindings)?;
    Ok(s(&p))
}

/// "Restore defaults": the file goes (kept as input.toml.bak), so Play
/// passes no PS3_INPUT_MAP again.
#[tauri::command]
fn reset_input_map() -> Result<(), String> {
    let p = input_map_path();
    if p.is_file() {
        fs::rename(&p, p.with_extension("toml.bak")).map_err(|e| e.to_string())?;
    }
    Ok(())
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ModsView {
    dir: String,
    dump_dir: String,
    packs: Vec<mods::ModPack>,
}

#[tauri::command]
fn list_mods() -> ModsView {
    let dir = mods_dir();
    ModsView { packs: mods::list(&dir), dir: s(&dir), dump_dir: s(&mods_dump_dir()) }
}

/// The screens in the system's order (NSScreen.screens: the first has the
/// menu bar), by the names the window host's PS3_DISPLAY matches.
#[tauri::command]
async fn list_screens() -> Vec<String> {
    if !cfg!(target_os = "macos") {
        return Vec::new();
    }
    tauri::async_runtime::spawn_blocking(|| {
        command_stdout(Command::new("/usr/bin/osascript").args([
            "-l",
            "JavaScript",
            "-e",
            "ObjC.import('AppKit'); JSON.stringify($.NSScreen.screens.js.map(s => s.localizedName.js))",
        ]))
        .map(|o| serde_json::from_str::<Vec<String>>(o.trim()).unwrap_or_default())
        .unwrap_or_default()
    })
    .await
    .unwrap_or_default()
}

#[tauri::command]
fn get_settings() -> Settings {
    load_settings_file()
}

/// launcher.json, and the launcher's keys of folklore_settings.ini.
#[tauri::command]
fn save_settings(app: AppHandle, settings: Settings) -> Result<(), String> {
    write_settings(&settings)?;
    if let Some(repo) = repo_root(&settings) {
        write_ini(&repo, &settings)?;
    }
    sync_app_icon(&app);
    Ok(())
}

#[tauri::command]
async fn find_repos() -> Vec<Builds> {
    tauri::async_runtime::spawn_blocking(|| find_repos_now().iter().map(|r| resolve_builds(r)).collect()).await.unwrap_or_default()
}

/// The in-game menu's saved settings (overlay.ini) moved aside to
/// overlay.ini.bak, so the launcher's and the runtime's defaults apply again.
#[tauri::command]
fn reset_overlay_ini() -> Result<(), String> {
    let repo = repo_root(&load_settings_file()).ok_or("no Folklore folder")?;
    let p = repo.join("overlay.ini");
    if p.is_file() {
        fs::rename(&p, repo.join("overlay.ini.bak")).map_err(|e| e.to_string())?;
    }
    Ok(())
}

/// Mount the saved disc image now (Game > Disc), for its art.
#[tauri::command]
async fn mount_disc() -> Result<String, String> {
    tauri::async_runtime::spawn_blocking(|| {
        let repo = repo_root(&load_settings_file()).ok_or("no Folklore folder")?;
        let g = saved_game(&repo).ok_or("no game saved yet (Install the game first)")?;
        let gp = Path::new(&g);
        if gp.is_dir() {
            return Ok(g);
        }
        mount_image(&repo, gp).map(|p| s(&p))
    })
    .await
    .map_err(|e| e.to_string())?
}

// ---------------------------------------------------------------------------
// The Dock and Finder icons from the player's disc (app_icon.rs)
// ---------------------------------------------------------------------------

fn app_icon_cache() -> PathBuf {
    data_root().join(app_icon::CACHE_NAME)
}

fn composed_icon(st: &Settings) -> Option<PathBuf> {
    if !st.game_icon {
        return None;
    }
    let repo = repo_root(st);
    app_icon::refresh(&app_icon_cache(), disc_art(repo.as_deref()).0.as_deref())
}

fn sync_app_icon(app: &AppHandle) {
    if !cfg!(target_os = "macos") {
        return;
    }
    let app = app.clone();
    thread::spawn(move || {
        let icon = composed_icon(&load_settings_file());
        let key = app_icon::key(icon.as_deref());
        let state = app.state::<AppState>();
        let mut cur = state.dock_icon.lock().unwrap();
        if *cur != key {
            *cur = key;
            app_icon::apply_dock_icon(&app, icon);
        }
    });
}

#[tauri::command]
async fn finder_icon(app: AppHandle, set: bool) -> Result<bool, String> {
    if !cfg!(target_os = "macos") {
        return Err("the Finder icon is macOS only".into());
    }
    let bundle = app_icon::app_bundle().ok_or("the launcher is not running from an .app (development build)")?;
    let icon = if set {
        let st = load_settings_file();
        let repo = repo_root(&st);
        Some(app_icon::refresh(&app_icon_cache(), disc_art(repo.as_deref()).0.as_deref()).ok_or("no ICON0.PNG from your disc yet (it is read from the mounted disc)")?)
    } else {
        None
    };
    let (tx, rx) = std::sync::mpsc::channel();
    let b = bundle.clone();
    app.run_on_main_thread(move || {
        let _ = tx.send(app_icon::set_finder_icon(&b, icon.as_deref()));
    })
    .map_err(|e| e.to_string())?;
    rx.recv().map_err(|e| e.to_string())??;
    Ok(app_icon::has_finder_icon(&bundle))
}

#[tauri::command]
fn take_play_request(state: State<'_, AppState>) -> bool {
    std::mem::take(&mut *state.play_request.lock().unwrap())
}

#[tauri::command]
fn take_start_view(state: State<'_, AppState>) -> Option<String> {
    state.start_view.lock().unwrap().take()
}

/// Home > Status > "Play shortcut on the Desktop" (play_shortcut.rs).
#[tauri::command]
async fn create_play_shortcut(app: AppHandle) -> Result<String, String> {
    if !cfg!(target_os = "macos") {
        return Err("the Play shortcut is macOS only".into());
    }
    let bundle = app_icon::app_bundle().ok_or("the launcher is not running from an .app (development build)")?;
    let icns = play_shortcut::bundle_icns(&bundle);
    let path = play_shortcut::write(&home().join("Desktop"), &app.config().identifier, icns.as_deref())?;
    if let Some(icon) = composed_icon(&load_settings_file()) {
        let (tx, rx) = std::sync::mpsc::channel();
        let p = path.clone();
        app.run_on_main_thread(move || {
            let _ = tx.send(app_icon::set_finder_icon(&p, Some(&icon)));
        })
        .map_err(|e| e.to_string())?;
        if let Ok(Err(e)) = rx.recv() {
            eprintln!("[launcher] Play shortcut icon: {e}");
        }
    }
    Ok(s(&path))
}

// ---------------------------------------------------------------------------
// Add to Steam (steam.rs, steam_art.rs)
// ---------------------------------------------------------------------------

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SteamStatus {
    available: bool,
    installed: bool,
    running: bool,
    users: usize,
    added: usize,
}

fn steam_dir() -> PathBuf {
    data_root().join("steam")
}

fn steam_target() -> steam::Target {
    steam::Target::new(&play_shortcut::executable(&steam_dir().join(play_shortcut::NAME)))
}

fn steam_available() -> bool {
    cfg!(target_os = "macos") && app_icon::app_bundle().is_some()
}

fn steam_art(st: &Settings) -> Option<steam::Art> {
    let repo = repo_root(st);
    let (icon0, pic1) = disc_art(repo.as_deref());
    let icon0 = app_icon::decode_png(&fs::read(icon0?).ok()?).ok()?;
    let pic1 = pic1.and_then(|p| fs::read(p).ok()).and_then(|b| app_icon::decode_png(&b).ok());
    let imgs = steam_art::draw(&icon0, pic1.as_ref());
    let enc = |i: &app_icon::Rgba| app_icon::encode_png(i).ok();
    Some([enc(&imgs.portrait)?, enc(&imgs.landscape)?, enc(&imgs.hero)?, enc(&imgs.icon)?])
}

#[tauri::command]
async fn steam_status() -> SteamStatus {
    tauri::async_runtime::spawn_blocking(|| {
        let users = steam::users(&steam::root(&home()));
        let t = steam_target();
        SteamStatus {
            available: steam_available(),
            installed: !users.is_empty(),
            running: steam::running(),
            users: users.len(),
            added: users.iter().filter(|u| steam::user_has(u, &t)).count(),
        }
    })
    .await
    .unwrap_or(SteamStatus { available: false, installed: false, running: false, users: 0, added: 0 })
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SteamAdded {
    users: usize,
    art: bool,
}

#[tauri::command]
async fn steam_add(app: AppHandle) -> Result<SteamAdded, String> {
    if !steam_available() {
        return Err("Add to Steam needs the packaged app on macOS".into());
    }
    let id = app.config().identifier.clone();
    tauri::async_runtime::spawn_blocking(move || {
        let users = steam::users(&steam::root(&home()));
        if users.is_empty() {
            return Err("Steam is not installed here, or nobody has signed in to it yet".to_string());
        }
        if steam::running() {
            return Err("steam-running".to_string());
        }
        let icns = app_icon::app_bundle().and_then(|b| play_shortcut::bundle_icns(&b));
        fs::create_dir_all(steam_dir()).map_err(|e| e.to_string())?;
        play_shortcut::write_kind(&steam_dir(), &id, icns.as_deref(), play_shortcut::Kind::Steam)?;
        let art = steam_art(&load_settings_file());
        let t = steam_target();
        for u in &users {
            steam::add(u, &t, art.as_ref())?;
        }
        Ok(SteamAdded { users: users.len(), art: art.is_some() })
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
async fn steam_remove(app: AppHandle) -> Result<usize, String> {
    let id = app.config().identifier.clone();
    tauri::async_runtime::spawn_blocking(move || {
        if steam::running() {
            return Err("steam-running".to_string());
        }
        let t = steam_target();
        let mut n = 0;
        for u in &steam::users(&steam::root(&home())) {
            n += steam::remove(u, &t)? as usize;
        }
        let bundle = steam_dir().join(play_shortcut::NAME);
        if play_shortcut::is_ours(&bundle, &id, play_shortcut::Kind::Steam) {
            fs::remove_dir_all(&bundle).map_err(|e| format!("{}: {e}", bundle.display()))?;
        }
        Ok(n)
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
async fn steam_quit() -> Result<(), String> {
    tauri::async_runtime::spawn_blocking(steam::quit).await.map_err(|e| e.to_string())?
}

// ---------------------------------------------------------------------------
// Requirements and the install / rebuild (setup_mac.sh)
// ---------------------------------------------------------------------------

/// One line of "What this computer needs". `level`: "ok", "warn", "fail",
/// "info". The window shows `key` (an i18n key under `prereq.msg.`, with
/// `vars`) and falls back to the English `message`; `path` is shown on a
/// line of its own.
#[derive(Serialize, Clone, Debug, Default)]
#[serde(rename_all = "camelCase")]
pub struct CheckItem {
    level: String,
    name: String,
    message: String,
    key: String,
    vars: BTreeMap<String, String>,
    path: Option<String>,
}

impl CheckItem {
    fn new(level: &str, name: &str, key: &str, message: impl Into<String>) -> Self {
        CheckItem { level: level.into(), name: name.into(), key: key.into(), message: message.into(), ..Default::default() }
    }
    fn at(level: &str, name: &str, path: &Path) -> Self {
        CheckItem::new(level, name, "", s(path)).path(path)
    }
    fn var(mut self, k: &str, v: impl ToString) -> Self {
        self.vars.insert(k.into(), v.to_string());
        self
    }
    fn path(mut self, p: &Path) -> Self {
        self.path = Some(s(p));
        self
    }
}

#[derive(Serialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct Prereqs {
    supported: bool,
    clt: bool,
    brew: bool,
    checks: Vec<CheckItem>,
    /// Nothing that stops setup_mac.sh ("fail") is left.
    ok: bool,
    repo: Option<String>,
    /// A game is chosen or saved, and it is there.
    game: bool,
    builds_ok: bool,
    free_gb: Option<u64>,
    need_gb: u64,
}

/// setup_mac.sh's Homebrew packages, with what shows each is installed.
const BREW_PKGS: &[&str] = &["cmake", "ninja", "sdl2", "glslang", "spirv-cross", "ffmpeg"];

fn free_gb(p: &Path) -> Option<u64> {
    let out = command_stdout(Command::new("/bin/df").arg("-k").arg(p))?;
    let kb: u64 = out.lines().nth(1)?.split_whitespace().nth(3)?.parse().ok()?;
    Some(kb / (1024 * 1024))
}

fn find_python() -> Option<PathBuf> {
    ["python3.14", "python3.13", "python3.12", "python3.11", "python3.10", "python3"].iter().find_map(|c| find_in_path(c))
}

#[tauri::command]
async fn check_prereqs() -> Prereqs {
    tauri::async_runtime::spawn_blocking(|| {
        let st = load_settings_file();
        let mut p = Prereqs { supported: host_supported(), clt: clt_installed(), ..Default::default() };
        if !p.supported {
            return p;
        }
        let mut checks = Vec::new();
        checks.push(if p.clt {
            CheckItem::new("ok", "xcode-clt", "cltOk", "installed")
        } else {
            CheckItem::new("fail", "xcode-clt", "cltMissing", "not installed (Apple's installer: the compiler, git)")
        });
        let brew = find_brew();
        p.brew = brew.is_some();
        checks.push(match &brew {
            Some(b) => CheckItem::at("ok", "brew", b),
            None => CheckItem::new("fail", "brew", "brewMissing", "not installed: setup_mac.sh installs the build tools with it"),
        });
        if brew.is_some() {
            let prefix = brew_prefix();
            let missing: Vec<&str> = BREW_PKGS.iter().copied().filter(|k| !prefix.join("opt").join(k).exists()).collect();
            checks.push(if missing.is_empty() {
                CheckItem::new("ok", "brewPkgs", "pkgsOk", BREW_PKGS.join(" ")).var("pkgs", BREW_PKGS.join(" "))
            } else {
                CheckItem::new("info", "brewPkgs", "pkgsLater", format!("the install adds: {}", missing.join(" "))).var("pkgs", missing.join(" "))
            });
        }
        checks.push(match find_python() {
            Some(py) => CheckItem::at("ok", "python", &py),
            None => CheckItem::new("warn", "python", "pythonMissing", "no python3 found (brew install python)"),
        });
        let repo = repo_root(&st);
        p.repo = repo.as_deref().map(s);
        match &repo {
            Some(r) => checks.push(CheckItem::at("ok", "repo", r)),
            None => checks.push(CheckItem::new("fail", "repo", "repoMissing", "no Folklore folder found: choose it below")),
        }
        if let Some(r) = &repo {
            let saved = saved_game(r);
            let chosen = Some(st.game_path.trim()).filter(|g| !g.is_empty()).map(str::to_string);
            let game = chosen.clone().or(saved.clone());
            checks.push(match &game {
                Some(g) if valid_game(Path::new(g)) => {
                    p.game = true;
                    let key = if chosen.is_some() && chosen != saved { "gameChosen" } else { "gameSaved" };
                    CheckItem::new("ok", "game", key, g.clone()).path(Path::new(g))
                }
                Some(g) => CheckItem::new("fail", "game", "gameGone", format!("not found: {g}")).var("path", g),
                None => CheckItem::new("fail", "game", "gameNone", "choose your disc image (.iso) or the folder that holds PS3_GAME"),
            });
            let b = resolve_builds(r);
            p.builds_ok = b.ok;
            p.need_gb = if b.ok { 0 } else { INSTALL_NEED_GB };
            p.free_gb = free_gb(r);
            if let Some(gb) = p.free_gb {
                checks.push(if p.need_gb == 0 {
                    CheckItem::new("ok", "disk", "diskEnough", format!("{gb} GB free: enough")).var("free", gb)
                } else if gb >= p.need_gb {
                    CheckItem::new("ok", "disk", "diskNeed", format!("{gb} GB free; the install needs about {} GB", p.need_gb)).var("free", gb).var("need", p.need_gb)
                } else {
                    CheckItem::new("warn", "disk", "diskLow", format!("only {gb} GB free; the install needs about {} GB", p.need_gb)).var("free", gb).var("need", p.need_gb)
                });
            }
            checks.push(if b.ok {
                CheckItem::new("ok", "build", "buildOk", s(Path::new(b.exe.as_deref().unwrap_or("")))).path(Path::new(b.exe.as_deref().unwrap_or("")))
            } else {
                CheckItem::new("info", "build", "buildMissing", format!("not built yet: {}", b.missing.join(", "))).var("missing", b.missing.join(", "))
            });
        }
        p.ok = !checks.iter().any(|c| c.level == "fail");
        p.checks = checks;
        p
    })
    .await
    .unwrap_or_default()
}

#[tauri::command]
fn install_clt() -> Result<(), String> {
    Command::new("/usr/bin/xcode-select")
        .arg("--install")
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .map(|_| ())
        .map_err(|e| format!("xcode-select: {e}"))
}

/// Stream a child's stdout and stderr to the window as `<event>-line` (and
/// into `log`), and its exit code as `<event>-exit`; AppState::rebuild holds
/// its pid while it runs.
fn stream_child(app: &AppHandle, mut child: std::process::Child, event: &'static str, log: Option<fs::File>) {
    use std::io::Write;
    let log = std::sync::Arc::new(Mutex::new(log));
    let (stdout, stderr) = (child.stdout.take().unwrap(), child.stderr.take().unwrap());
    let (a1, l1) = (app.clone(), log.clone());
    let t1 = thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            if let Some(f) = l1.lock().unwrap().as_mut() {
                let _ = writeln!(f, "{line}");
            }
            let _ = a1.emit(&format!("{event}-line"), line);
        }
    });
    let (a2, l2) = (app.clone(), log);
    let t2 = thread::spawn(move || {
        for line in BufReader::new(stderr).lines().map_while(Result::ok) {
            if let Some(f) = l2.lock().unwrap().as_mut() {
                let _ = writeln!(f, "{line}");
            }
            let _ = a2.emit(&format!("{event}-line"), line);
        }
    });
    let a3 = app.clone();
    thread::spawn(move || {
        let code = child.wait().map(|s| s.code().unwrap_or(-1)).unwrap_or(-1);
        // the last lines reach the window before the exit
        let _ = t1.join();
        let _ = t2.join();
        *a3.state::<AppState>().rebuild.lock().unwrap() = None;
        let _ = a3.emit(&format!("{event}-exit"), code);
    });
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SetupRequest {
    /// A disc image or folder to install from (`setup_mac.sh <game>`, saved
    /// in folklore_mac.env). None: the saved one (`setup_mac.sh`).
    game: Option<String>,
    jobs: Option<u32>,
    no_promote: Option<bool>,
    relift: Option<bool>,
}

/// Install the game or rebuild it: `setup_mac.sh [<game>]` in the repository
/// with zsh (each step it finished before is skipped). Errors the window
/// explains itself: "clt", "brew", "no-game".
#[tauri::command]
fn start_setup(app: AppHandle, state: State<'_, AppState>, req: SetupRequest) -> Result<String, String> {
    require_supported_host()?;
    if state.game.lock().unwrap().is_some() {
        return Err("the game is running: close it first".into());
    }
    let mut slot = state.rebuild.lock().unwrap();
    if slot.is_some() {
        return Err("an install or a rebuild is already running".into());
    }
    if !clt_installed() {
        return Err("clt".into());
    }
    if find_brew().is_none() {
        return Err("brew".into());
    }
    let st = load_settings_file();
    let repo = repo_root(&st).ok_or("no Folklore folder: choose it in Advanced > Install / rebuild")?;
    let game = req.game.as_deref().map(str::trim).filter(|g| !g.is_empty()).map(PathBuf::from);
    match &game {
        Some(g) if !valid_game(g) => return Err(format!("{}: not a disc image or a folder holding PS3_GAME", g.display())),
        None if !env_file(&repo).is_file() => return Err("no-game".into()),
        _ => {}
    }
    let mut cmd = Command::new("/usr/bin/nice");
    cmd.args(["-n", "10", "/bin/zsh"]).arg(repo.join("setup_mac.sh"));
    if let Some(g) = &game {
        cmd.arg(g);
    }
    cmd.current_dir(&repo)
        .env("PATH", tool_path())
        .env("TERM", "dumb")
        .env("HOMEBREW_NO_AUTO_UPDATE", "1")
        .env("NONINTERACTIVE", "1")
        .env_remove("JOBS")
        .env_remove("NO_PROMOTE")
        .env_remove("RELIFT")
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    let jobs = req.jobs.unwrap_or(st.jobs);
    if jobs > 0 {
        cmd.env("JOBS", jobs.min(64).to_string());
    }
    if req.no_promote.unwrap_or(st.no_promote) {
        cmd.env("NO_PROMOTE", "1");
    }
    if req.relift.unwrap_or(st.relift) {
        cmd.env("RELIFT", "1");
    }
    own_process_group(&mut cmd);
    let _ = fs::create_dir_all(data_root());
    let log = fs::File::create(install_log_path()).ok();
    let child = cmd.spawn().map_err(|e| format!("zsh: {e}"))?;
    *slot = Some(child.id());
    drop(slot);
    let first = format!("$ cd {} && ./setup_mac.sh{}", shell_quote(&s(&repo)), game.as_deref().map(|g| format!(" {}", shell_quote(&s(g)))).unwrap_or_default());
    if let Some(mut f) = log.as_ref().and_then(|f| f.try_clone().ok()) {
        use std::io::Write;
        let _ = writeln!(f, "{first}");
    }
    let _ = app.emit("rebuild-line", first);
    stream_child(&app, child, "rebuild", log);
    Ok(s(&repo))
}

#[tauri::command]
fn cancel_rebuild(state: State<'_, AppState>) {
    if let Some(pid) = *state.rebuild.lock().unwrap() {
        kill_tree(pid, false);
    }
}

// ---------------------------------------------------------------------------
// Updates (update.rs)
// ---------------------------------------------------------------------------

#[derive(Serialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct UpdateInfo {
    sources: update::Sources,
    /// The newest commit touching the port is newer than the build.
    rebuild_needed: bool,
    built_at: u64,
    app: update::AppUpdate,
}

#[tauri::command]
async fn check_updates(app: AppHandle, online: bool) -> UpdateInfo {
    tauri::async_runtime::spawn_blocking(move || {
        let st = load_settings_file();
        let repo = repo_root(&st);
        let b = repo.as_deref().map(resolve_builds);
        let sources = repo.as_deref().map(|r| update::sources(r, online)).unwrap_or_default();
        let built_at = b.as_ref().map_or(0, |b| b.built_at);
        let rebuild_needed = built_at > 0 && sources.sources_time > built_at;
        let app = update::check_app(&app, online, built_at);
        UpdateInfo { sources, rebuild_needed, built_at, app }
    })
    .await
    .unwrap_or_default()
}

/// "Get the new sources": git pull --ff-only in the repository, streamed as
/// rebuild-line / rebuild-exit.
#[tauri::command]
fn pull_sources(app: AppHandle, state: State<'_, AppState>) -> Result<(), String> {
    let mut slot = state.rebuild.lock().unwrap();
    if slot.is_some() {
        return Err("an install or a rebuild is running".into());
    }
    let repo = repo_root(&load_settings_file()).ok_or("no Folklore folder")?;
    let mut cmd = Command::new("/usr/bin/git");
    cmd.arg("-C")
        .arg(&repo)
        .args(["pull", "--ff-only"])
        .env("GIT_TERMINAL_PROMPT", "0")
        .env("PATH", tool_path())
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    own_process_group(&mut cmd);
    let child = cmd.spawn().map_err(|e| format!("git: {e}"))?;
    *slot = Some(child.id());
    drop(slot);
    let _ = app.emit("rebuild-line", format!("$ git -C {} pull --ff-only", shell_quote(&s(&repo))));
    stream_child(&app, child, "rebuild", None);
    Ok(())
}

// ---------------------------------------------------------------------------
// Links, files, the logs
// ---------------------------------------------------------------------------

const URL_ALLOWED: &[&str] = &["https://brew.sh/", "https://github.com/erikomis/folklore-decomp", "https://github.com/sp00nznet/ps3recomp"];

#[tauri::command]
fn open_url(url: String) -> Result<(), String> {
    if !URL_ALLOWED.iter().any(|p| url.starts_with(p)) || url.chars().any(|c| c.is_whitespace() || c == '"') {
        return Err(format!("{url}: not a link this app opens"));
    }
    Command::new("/usr/bin/open").arg(&url).spawn().map(|_| ()).map_err(|e| e.to_string())
}

/// Show a folder or file in Finder (never opens an .app: that would run it).
#[tauri::command]
fn reveal(path: String) -> Result<(), String> {
    let p = PathBuf::from(&path);
    if !p.exists() {
        let parent = p.parent().map(Path::to_path_buf).unwrap_or(p);
        return Command::new("/usr/bin/open").arg(&parent).spawn().map(|_| ()).map_err(|e| e.to_string());
    }
    if p.is_dir() && p.extension().is_none_or(|e| e != "app") {
        Command::new("/usr/bin/open").arg(&p).spawn().map(|_| ()).map_err(|e| e.to_string())
    } else {
        Command::new("/usr/bin/open").arg("-R").arg(&p).spawn().map(|_| ()).map_err(|e| e.to_string())
    }
}

fn tail_text(data: &[u8], cut: bool) -> String {
    let start = if cut { data.iter().position(|&b| b == b'\n').map_or(0, |i| i + 1) } else { 0 };
    String::from_utf8_lossy(&data[start..]).into_owned()
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct GameLog {
    path: String,
    exists: bool,
    size: u64,
    modified: u64,
    text: String,
    truncated: bool,
}

/// The end of logs/run-mac.log (the game's output, as run_mac.sh writes it).
#[tauri::command]
fn read_game_log(max_bytes: Option<u64>) -> Result<GameLog, String> {
    let repo = repo_root(&load_settings_file()).ok_or("no Folklore folder")?;
    let path = game_log_path(&repo);
    let max = max_bytes.unwrap_or(512 * 1024).clamp(4 * 1024, 8 * 1024 * 1024);
    let mut out = GameLog { path: s(&path), exists: false, size: 0, modified: 0, text: String::new(), truncated: false };
    let Ok(mut f) = fs::File::open(&path) else { return Ok(out) };
    let meta = f.metadata().map_err(|e| e.to_string())?;
    out.exists = true;
    out.size = meta.len();
    out.modified = mtime(&path);
    out.truncated = meta.len() > max;
    if out.truncated {
        f.seek(SeekFrom::Start(meta.len() - max)).map_err(|e| e.to_string())?;
    }
    let mut buf = Vec::with_capacity(max.min(meta.len()) as usize);
    f.take(max).read_to_end(&mut buf).map_err(|e| e.to_string())?;
    out.text = tail_text(&buf, out.truncated);
    Ok(out)
}

#[tauri::command]
fn log_paths() -> BTreeMap<String, String> {
    let st = load_settings_file();
    let repo = repo_root(&st);
    let mut m = BTreeMap::new();
    if let Some(r) = &repo {
        m.insert("game".into(), s(&game_log_path(r)));
        m.insert("logs".into(), s(&r.join("logs")));
        m.insert("repo".into(), s(r));
        m.insert("play".into(), s(&play_log_path(r)));
    }
    if let Some(root) = save_locations(&st, repo.as_deref()).roots.first() {
        m.insert("saves".into(), s(&root.dir));
    }
    m.insert("data".into(), s(&data_root()));
    m.insert("install".into(), s(&install_log_path()));
    m.insert("reports".into(), s(&reports_dir()));
    m
}

fn command_line(program: &str, args: &[&str]) -> String {
    command_stdout(Command::new(program).args(args)).map(|s| s.trim().to_string()).unwrap_or_default()
}

/// UTC "YYYY-MM-DD HH:MM:SS".
fn utc_text(secs: u64) -> String {
    let st = saves::stamp(secs);
    format!("{}-{}-{} {}:{}:{}", &st[0..4], &st[4..6], &st[6..8], &st[9..11], &st[11..13], &st[13..15])
}

/// system.txt of a log archive: the Mac, the repository, the build and the
/// command Play uses.
fn system_info(st: &Settings, repo: Option<&Path>, now: u64) -> String {
    let mut t = String::new();
    let mut line = |k: &str, v: &str| {
        if !v.is_empty() {
            t.push_str(&format!("{k}: {v}\n"));
        }
    };
    line("created", &utc_text(now));
    line("launcher", env!("CARGO_PKG_VERSION"));
    line("macos", &format!("{} ({})", command_line("/usr/bin/sw_vers", &["-productVersion"]), command_line("/usr/bin/sw_vers", &["-buildVersion"])));
    line("model", &command_line("/usr/sbin/sysctl", &["-n", "hw.model"]));
    line("cpu", &command_line("/usr/sbin/sysctl", &["-n", "machdep.cpu.brand_string"]));
    let mem = command_line("/usr/sbin/sysctl", &["-n", "hw.memsize"]).parse::<u64>().unwrap_or(0);
    if mem > 0 {
        line("memory", &format!("{} GB", mem >> 30));
    }
    if let Some(r) = repo {
        let b = resolve_builds(r);
        let g = game_info(r);
        line("repository", &s(r));
        line("commit", &command_line("/usr/bin/git", &["-C", &s(r), "rev-parse", "--short", "HEAD"]));
        line("built", &if b.built_at > 0 { utc_text(b.built_at) } else { "no".into() });
        line("lifted from", &b.region);
        line("disc", &format!("{} {} {}", g.title_id, g.title, g.app_ver).trim().to_string());
        t.push_str("\nPlay runs:\n  ");
        let env = launch_env(st, &input_env());
        t.push_str(&launch_argv(st, r, &env).iter().map(|a| shell_quote(a)).collect::<Vec<_>>().join(" "));
        t.push('\n');
    }
    t
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LogsZip {
    path: String,
    bytes: u64,
    files: usize,
}

/// Advanced > Debug > "Create .zip of the logs": logs/*.log of the
/// repository (the last 8 MB of each), the launcher's install.log,
/// system.txt and the optional description as notes.txt, with the home
/// folder's path written as `~`. Into <data>/reports/.
#[tauri::command]
async fn zip_logs(notes: Option<String>) -> Result<LogsZip, String> {
    tauri::async_runtime::spawn_blocking(move || {
        let st = load_settings_file();
        let repo = repo_root(&st);
        let now = now_secs();
        let home = s(&home());
        let scrub = |t: String| if home.len() > 1 { t.replace(&home, "~") } else { t };
        let mut entries = Vec::new();
        let mut add = |name: String, p: &Path| {
            let Ok(mut f) = fs::File::open(p) else { return };
            let len = f.metadata().map(|m| m.len()).unwrap_or(0);
            const MAX: u64 = 8 << 20;
            if len > MAX {
                let _ = f.seek(SeekFrom::Start(len - MAX));
            }
            let mut buf = Vec::new();
            if f.take(MAX).read_to_end(&mut buf).is_ok() {
                let text = scrub(tail_text(&buf, len > MAX));
                entries.push(zipfile::Entry { name, data: text.into_bytes(), mtime: mtime(p) });
            }
        };
        if let Some(r) = &repo {
            let mut logs: Vec<PathBuf> = fs::read_dir(r.join("logs")).into_iter().flatten().flatten().map(|e| e.path()).filter(|p| p.extension().is_some_and(|e| e == "log")).collect();
            logs.sort();
            for p in &logs {
                add(format!("logs/{}", p.file_name().unwrap().to_string_lossy()), p);
            }
        }
        add("install.log".into(), &install_log_path());
        if entries.is_empty() {
            return Err("no logs yet: they appear after the first install or Play".to_string());
        }
        entries.push(zipfile::Entry { name: "system.txt".into(), data: scrub(system_info(&st, repo.as_deref(), now)).into_bytes(), mtime: now });
        if let Some(n) = notes.as_deref().map(str::trim).filter(|n| !n.is_empty()) {
            entries.push(zipfile::Entry { name: "notes.txt".into(), data: format!("{n}\n").into_bytes(), mtime: now });
        }
        let dir = reports_dir();
        fs::create_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
        let dest = dir.join(format!("folklore-logs-{}.zip", saves::stamp(now)));
        let files = entries.len();
        zipfile::write(&dest, &entries)?;
        Ok(LogsZip { bytes: fs::metadata(&dest).map(|m| m.len()).unwrap_or(0), path: s(&dest), files })
    })
    .await
    .map_err(|e| e.to_string())?
}

// ---------------------------------------------------------------------------
// Shader cache
// ---------------------------------------------------------------------------

fn dir_usage(dir: &Path) -> (u64, u64) {
    let (mut bytes, mut files) = (0u64, 0u64);
    if let Ok(rd) = fs::read_dir(dir) {
        for e in rd.flatten() {
            match e.metadata() {
                Ok(m) if m.is_dir() => {
                    let (b, f) = dir_usage(&e.path());
                    bytes += b;
                    files += f;
                }
                Ok(m) => {
                    bytes += m.len();
                    files += 1;
                }
                Err(_) => {}
            }
        }
    }
    (bytes, files)
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ShaderCacheInfo {
    path: Option<String>,
    bytes: u64,
    files: u64,
}

fn shader_cache_info_now() -> ShaderCacheInfo {
    let st = load_settings_file();
    match shader_cache_dir(&st, repo_root(&st).as_deref()) {
        Some(d) => {
            let (bytes, files) = dir_usage(&d);
            ShaderCacheInfo { path: Some(s(&d)), bytes, files }
        }
        None => ShaderCacheInfo { path: None, bytes: 0, files: 0 },
    }
}

#[tauri::command]
fn shader_cache_info() -> ShaderCacheInfo {
    shader_cache_info_now()
}

/// Empty the shader cache folder (run_mac.sh makes it again on Play).
#[tauri::command]
fn shader_cache_clear(state: State<'_, AppState>) -> Result<ShaderCacheInfo, String> {
    if game_busy(&state) {
        return Err("the game is running: close it before clearing the shader cache".into());
    }
    let st = load_settings_file();
    if let Some(d) = shader_cache_dir(&st, repo_root(&st).as_deref()) {
        if d.exists() {
            fs::remove_dir_all(&d).map_err(|e| format!("{}: {e}", d.display()))?;
        }
    }
    Ok(shader_cache_info_now())
}

// ---------------------------------------------------------------------------
// Saves (saves.rs)
// ---------------------------------------------------------------------------

fn game_busy(state: &AppState) -> bool {
    state.game.lock().unwrap().is_some() || process_running(GAME_EXE) || process_running(HOST_EXE)
}

fn with_saves<T>(state: &State<'_, AppState>, f: impl FnOnce(&saves::Locations) -> Result<T, String>) -> Result<T, String> {
    let game = state.game.lock().unwrap();
    if game.is_some() || process_running(GAME_EXE) || process_running(HOST_EXE) {
        return Err("the game is running: close it before changing saves".into());
    }
    let st = load_settings_file();
    let loc = save_locations(&st, repo_root(&st).as_deref());
    if loc.roots.is_empty() {
        return Err("no Folklore folder".into());
    }
    let r = f(&loc);
    drop(game);
    r
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SavesOverview {
    slots: Vec<Slot>,
    backups: Vec<BackupMeta>,
    /// The save folders (label, path), the main one first.
    dirs: Vec<(String, String)>,
    backups_dir: String,
    game_running: bool,
}

#[tauri::command]
async fn saves_overview(state: State<'_, AppState>) -> Result<SavesOverview, String> {
    let running = game_busy(&state);
    tauri::async_runtime::spawn_blocking(move || {
        let st = load_settings_file();
        let loc = save_locations(&st, repo_root(&st).as_deref());
        SavesOverview {
            slots: saves::list_slots(&loc, true),
            backups: saves::list_backups(&loc),
            dirs: loc.roots.iter().map(|r| (r.label.clone(), s(&r.dir))).collect(),
            backups_dir: s(&loc.backups),
            game_running: running,
        }
    })
    .await
    .map_err(|e| e.to_string())
}

#[tauri::command]
async fn saves_backup(state: State<'_, AppState>, key: Option<String>) -> Result<Option<BackupMeta>, String> {
    with_saves(&state, |loc| {
        let one = key.map(|k| vec![k]);
        saves::create_backup(loc, one.as_deref(), "manual", now_secs())
    })
}

#[tauri::command]
async fn saves_restore(state: State<'_, AppState>, id: String, key: Option<String>) -> Result<saves::Report, String> {
    with_saves(&state, |loc| saves::restore_backup(loc, &id, key.as_deref(), now_secs()))
}

#[tauri::command]
async fn saves_delete_backup(state: State<'_, AppState>, id: String) -> Result<(), String> {
    with_saves(&state, |loc| saves::delete_backup(loc, &id))
}

#[tauri::command]
async fn saves_delete_slot(state: State<'_, AppState>, key: String) -> Result<BackupMeta, String> {
    with_saves(&state, |loc| saves::delete_slot(loc, &key, now_secs()))
}

#[tauri::command]
async fn saves_export_zip(state: State<'_, AppState>, keys: Option<Vec<String>>, dest: String) -> Result<String, String> {
    let mut dest = PathBuf::from(dest.trim());
    if dest.as_os_str().is_empty() {
        return Err("no file chosen".into());
    }
    if !dest.extension().is_some_and(|e| e.eq_ignore_ascii_case("zip")) {
        dest.as_mut_os_string().push(".zip");
    }
    with_saves(&state, |loc| saves::export_zip(loc, keys.as_deref(), &dest, now_secs()))?;
    Ok(s(&dest))
}

#[tauri::command]
async fn saves_import_zip(state: State<'_, AppState>, path: String) -> Result<saves::Report, String> {
    with_saves(&state, |loc| saves::import_zip(loc, Path::new(path.trim()), now_secs()))
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(AppState::default())
        .setup(|app| {
            sync_app_icon(app.handle());
            let args: Vec<String> = std::env::args().collect();
            let st = app.state::<AppState>();
            *st.play_request.lock().unwrap() = play_shortcut::requested(args.iter().cloned());
            *st.exit_with_game.lock().unwrap() = play_shortcut::exit_requested(&args);
            *st.start_view.lock().unwrap() = play_shortcut::start_view(&args);
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            get_settings,
            save_settings,
            get_input_map,
            save_input_map,
            reset_input_map,
            list_mods,
            list_screens,
            get_status,
            find_repos,
            reset_overlay_ini,
            mount_disc,
            check_updates,
            pull_sources,
            update::install_update,
            update::restart_app,
            check_prereqs,
            install_clt,
            start_setup,
            cancel_rebuild,
            open_url,
            launch_preview,
            play,
            stop_game,
            reveal,
            log_paths,
            read_game_log,
            zip_logs,
            shader_cache_info,
            shader_cache_clear,
            saves_overview,
            saves_backup,
            saves_restore,
            saves_delete_backup,
            saves_delete_slot,
            saves_export_zip,
            saves_import_zip,
            finder_icon,
            take_play_request,
            take_start_view,
            create_play_shortcut,
            steam_status,
            steam_add,
            steam_remove,
            steam_quit
        ])
        .build(tauri::generate_context!())
        .expect("error while running the launcher")
        .run(|app, ev| {
            // Quitting stops an install or a rebuild (its own process group,
            // by pid); setup_mac.sh skips what it finished next time.
            if let tauri::RunEvent::Exit = ev {
                if let Some(pid) = *app.state::<AppState>().rebuild.lock().unwrap() {
                    kill_tree(pid, false);
                }
            }
        });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("fl-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn base64_matches_rfc4648() {
        assert_eq!(base64(b""), "");
        assert_eq!(base64(b"f"), "Zg==");
        assert_eq!(base64(b"foobar"), "Zm9vYmFy");
    }

    #[test]
    fn env_lines() {
        let v = parse_env_lines("# c\nA=1\nexport B=\"x y\"\nbad key=2\n\nC=\n1X=2");
        assert_eq!(v, vec![("A".into(), "1".into()), ("B".into(), "x y".into()), ("C".into(), "".into())]);
    }

    #[test]
    fn zsh_words() {
        assert_eq!(zsh_unquote(r"/Users/a/Folklore\ \(USA\)/F.iso").as_deref(), Some("/Users/a/Folklore (USA)/F.iso"));
        assert_eq!(zsh_unquote("'/a b/c'").as_deref(), Some("/a b/c"));
        assert_eq!(zsh_unquote(r#""/a \"b\"""#).as_deref(), Some("/a \"b\""));
        assert_eq!(zsh_unquote("$'/a\\nb'"), None);
    }

    #[test]
    fn saved_game_from_env_file() {
        let d = tmp("env");
        fs::write(d.join("folklore_mac.env"), "FOLKLORE_GAME=/x/Folklore\\ \\(USA\\).iso\n").unwrap();
        assert_eq!(saved_game(&d).as_deref(), Some("/x/Folklore (USA).iso"));
        fs::remove_dir_all(d).unwrap();
    }

    #[test]
    fn ini_keeps_other_lines() {
        let text = "# Folklore port settings\nframe_rate_cap = original\nrender_scale = 2\ngame_dir = C:\\x\n";
        let mut st = Settings::default();
        ini_into_settings(&ini_values(text), &mut st);
        assert_eq!(st.frame_rate_cap, "original");
        st.frame_rate_cap = "60".into();
        st.logo_skip = "always".into();
        st.motion_controls = false;
        let out = ini_apply(text, &ini_wanted(&st));
        assert!(out.starts_with("# Folklore port settings\nframe_rate_cap = 60\nrender_scale = 2\ngame_dir = C:\\x\n"), "{out}");
        assert!(out.contains("\nskip_logos = always\n") && out.contains("\nmotion_controls = off\n"), "{out}");
        let mut back = Settings::default();
        ini_into_settings(&ini_values(&out), &mut back);
        assert_eq!((back.frame_rate_cap.as_str(), back.logo_skip.as_str(), back.motion_controls), ("60", "always", false));
        // nothing to change: the same text
        assert_eq!(ini_apply(&out, &ini_wanted(&st)), out);
        // the engine's older forms
        let mut old = Settings::default();
        ini_into_settings(&ini_values("skip_logos = 1\nmotion_controls = auto_capture\nlanguage = French\n"), &mut old);
        assert_eq!((old.logo_skip.as_str(), old.motion_controls, old.language.as_str()), ("start", false, "french"));
    }

    #[test]
    fn defaults_pass_nothing() {
        let st = Settings::default();
        assert!(launch_env(&st, &InputEnv::default()).is_empty());
        let mut st2 = Settings {
            window_mode: "borderless".into(),
            window_size: "1920x1080".into(),
            display: "LG".into(),
            upscale: "metalfx".into(),
            aa: "smaa".into(),
            shader_cache: false,
            ..Settings::default()
        };
        st2.extra_env = "PS3_AA=fxaa".into();
        let e = launch_env(&st2, &InputEnv::default());
        let keys: Vec<&str> = e.iter().map(|(k, _)| k.as_str()).collect();
        assert_eq!(keys, ["PS3_WINDOW_MODE", "PS3_WINDOW_SIZE", "PS3_DISPLAY", "PS3_UPSCALE", "PS3_AA", "PS3_SHADER_CACHE", "PS3_AA"]);
        let argv = launch_argv(&Settings { keep_awake: false, ..st2.clone() }, Path::new("/r"), &e);
        assert_eq!(&argv[..3], ["/bin/zsh", "/r/run_mac.sh", "PS3_WINDOW_MODE=borderless"]);
        // the game's own window: no host options, its own fullscreen and size
        let own = launch_env(&Settings { host_window: false, ..st2 }, &InputEnv::default());
        let keys: Vec<&str> = own.iter().map(|(k, _)| k.as_str()).collect();
        assert_eq!(keys, ["FOLKLORE_HOST_WINDOW", "PS3RECOMP_FULLSCREEN", "PS3RECOMP_METAL_WINDOW", "PS3_AA", "PS3_SHADER_CACHE", "PS3_AA"]);
        assert!(!valid_window_size("10x10") && valid_window_size("1280x720"));
    }

    #[test]
    fn repo_and_builds() {
        let r = tmp("repo");
        assert!(!is_repo(&r));
        for f in ["setup_mac.sh", "run_mac.sh"] {
            fs::write(r.join(f), "").unwrap();
        }
        fs::create_dir_all(r.join("folklore/build-mac")).unwrap();
        assert!(is_repo(&r));
        let b = resolve_builds(&r);
        assert!(!b.ok);
        assert_eq!(b.missing, ["folklore_mac.env", "folklore/build-mac/folklore"]);
        fs::write(r.join("folklore/build-mac/folklore"), "").unwrap();
        fs::write(r.join("folklore_mac.env"), "FOLKLORE_GAME=/nowhere.iso\n").unwrap();
        assert!(resolve_builds(&r).ok);
        let g = game_info(&r);
        assert!(g.env_file && !g.exists && g.dir.is_none());
        fs::remove_dir_all(r).unwrap();
    }
}
