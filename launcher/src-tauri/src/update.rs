//! Updates: the port's sources (the checkout the builds come from) and the
//! launcher itself.
//!
//! - **Sources.** When the Folklore repository is a git checkout with an
//!   upstream, `git fetch` and the count of commits it is behind; "Get the
//!   new sources" is `git pull --ff-only` (never a merge, never a reset: a
//!   checkout with changes of its own is left to the player). The build is
//!   "older than the sources" when the newest commit touching what goes into
//!   it (folklore/, ps3recomp/, patches/, the mac scripts) is newer than
//!   folklore/build-mac/folklore.
//! - **New launcher version.** The update manifest (`latest.json`, the shape
//!   tauri-plugin-updater reads), compared with this app's version, fetched
//!   with the system's `curl` from the first of `ENDPOINTS` that has one.
//!   **No release feed exists for this launcher yet**: `ENDPOINTS` is empty,
//!   so the check answers "not set up" (`AppUpdate::feed` false) without
//!   touching the network, and "Download and install" refuses with
//!   `blocked:nofeed`. To turn it on later: publish signed releases with a
//!   `latest.json`, list its URLs here, and add tauri-plugin-updater back
//!   (its `plugins.updater` config with the public key in tauri.conf.json)
//!   in place of the refusal in `install_update`.
//! - **Rebuild after an update.** An install would leave `app-updated.json`
//!   (from, to, when) in the launcher's folder; while this app is that
//!   version and the build is older than the update, Updates and Home offer
//!   "Rebuild the game".
//!
//! `FOLKLORE_NO_UPDATE_CHECK=1` turns the launcher check off; the setting
//! "Check for updates when the app opens" only skips the check at start.

use serde::{Deserialize, Serialize};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use tauri::{AppHandle, Manager};

fn git(root: &Path, args: &[&str]) -> Option<String> {
    let out = crate::no_console(&mut Command::new("git"))
        .arg("-C")
        .arg(root)
        .args(args)
        .env("GIT_TERMINAL_PROMPT", "0")
        .stdin(Stdio::null())
        .stderr(Stdio::null())
        .output()
        .ok()?;
    out.status.success().then(|| String::from_utf8_lossy(&out.stdout).trim().to_string())
}

#[derive(Serialize, Default, Debug)]
#[serde(rename_all = "camelCase")]
pub struct Sources {
    /// The port folder is a git checkout.
    pub git: bool,
    pub branch: String,
    pub commit: String,
    pub subject: String,
    /// Seconds since the epoch of the commit checked out.
    pub commit_time: u64,
    /// The upstream it follows, and how far behind/ahead it is (after a fetch).
    pub upstream: String,
    pub behind: u32,
    pub ahead: u32,
    /// Files changed in the working tree (git status --porcelain, tracked only).
    pub dirty: u32,
    /// The newest commit touching what goes into the builds.
    pub sources_time: u64,
    pub fetched: bool,
    pub error: Option<String>,
}

/// The checkout's state; `fetch` asks the remote first (network).
pub fn sources(root: &Path, fetch: bool) -> Sources {
    let mut s = Sources::default();
    if git(root, &["rev-parse", "--is-inside-work-tree"]).as_deref() != Some("true") {
        return s;
    }
    s.git = true;
    s.branch = git(root, &["rev-parse", "--abbrev-ref", "HEAD"]).unwrap_or_default();
    s.commit = git(root, &["rev-parse", "--short", "HEAD"]).unwrap_or_default();
    s.subject = git(root, &["log", "-1", "--format=%s"]).unwrap_or_default();
    s.commit_time = git(root, &["log", "-1", "--format=%ct"]).and_then(|v| v.parse().ok()).unwrap_or(0);
    s.sources_time = git(root, &["log", "-1", "--format=%ct", "--", "folklore", "ps3recomp", "patches", "setup_mac.sh", "mac_common.sh"])
        .and_then(|v| v.parse().ok())
        .unwrap_or(0);
    s.dirty = git(root, &["status", "--porcelain", "--untracked-files=no"]).map(|v| v.lines().count() as u32).unwrap_or(0);
    s.upstream = git(root, &["rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{u}"]).unwrap_or_default();
    if s.upstream.is_empty() {
        return s;
    }
    if fetch {
        let remote = s.upstream.split('/').next().unwrap_or("origin").to_string();
        match crate::no_console(&mut Command::new("git"))
            .arg("-C")
            .arg(root)
            .args(["fetch", "--quiet", &remote])
            .env("GIT_TERMINAL_PROMPT", "0")
            .env("GIT_SSH_COMMAND", "ssh -o BatchMode=yes -o ConnectTimeout=10")
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::piped())
            .output()
        {
            Ok(o) if o.status.success() => s.fetched = true,
            Ok(o) => s.error = Some(String::from_utf8_lossy(&o.stderr).trim().lines().last().unwrap_or("git fetch failed").to_string()),
            Err(e) => s.error = Some(format!("git: {e}")),
        }
    }
    if let Some(v) = git(root, &["rev-list", "--left-right", "--count", "HEAD...@{u}"]) {
        let mut it = v.split_whitespace().map(|n| n.parse::<u32>().unwrap_or(0));
        s.ahead = it.next().unwrap_or(0);
        s.behind = it.next().unwrap_or(0);
    }
    s
}


// ---------------------------------------------------------------------------
// The launcher: latest.json, the install, the restart
// ---------------------------------------------------------------------------

/// Where the update manifest is looked for, in order. Empty: this launcher
/// has no release feed yet (see the module's notes).
pub const ENDPOINTS: [&str; 0] = [];
/// The release pages (open_url allows the repository).
const RELEASES_PAGE: &str = "https://github.com/erikomis/folklore-decomp/releases";
/// The manifest's entry for this build.
const PLATFORM: &str = "darwin-aarch64";
/// What an install leaves for the next start (in the launcher's folder).
const MARKER: &str = "app-updated.json";

#[derive(Serialize, Default, Debug)]
#[serde(rename_all = "camelCase")]
pub struct AppUpdate {
    pub current: String,
    /// A release feed is configured (ENDPOINTS is not empty).
    pub feed: bool,
    /// The online check ran (it is skipped when not asked, turned off, or
    /// when there is no feed).
    pub checked: bool,
    pub latest: Option<String>,
    pub newer: bool,
    /// The release page of `latest`.
    pub url: Option<String>,
    pub notes: Option<String>,
    pub pub_date: Option<String>,
    /// FOLKLORE_NO_UPDATE_CHECK is set: no check.
    pub disabled: bool,
    pub error: Option<String>,
    /// Why "Download and install" cannot work here (None: it can):
    /// "nofeed" (no release feed yet), "platform", "dev" (not a packaged
    /// .app), "dmg", "translocated".
    pub install_blocker: Option<String>,
    /// A version installed in this session, waiting for the restart.
    pub installed_pending: Option<String>,
    /// This app was updated from that version, after the build was made.
    pub updated_from: Option<String>,
}

/// "1.2.3" (a leading v allowed) -> (1, 2, 3).
pub fn parse_version(v: &str) -> Option<(u32, u32, u32)> {
    let v = v.trim().trim_start_matches('v');
    let core = v.split(['-', '+']).next()?;
    let mut it = core.split('.').map(|p| p.parse::<u32>().ok());
    let r = (it.next()??, it.next().unwrap_or(Some(0))?, it.next().unwrap_or(Some(0))?);
    it.next().is_none().then_some(r)
}

/// Whether `latest` is newer than `current` (semver: a pre-release such as
/// `1.0.0-rc1` comes before `1.0.0`).
pub fn is_newer(latest: &str, current: &str) -> bool {
    match (parse_version(latest), parse_version(current)) {
        (Some(a), Some(b)) if a != b => a > b,
        (Some(_), Some(_)) => {
            let pre = |v: &str| v.split('+').next().unwrap_or("").contains('-');
            pre(current) && !pre(latest)
        }
        _ => false,
    }
}

/// What the app shows of an update manifest.
#[derive(Debug, PartialEq)]
struct Manifest {
    version: String,
    notes: Option<String>,
    pub_date: Option<String>,
    /// It has a download for this build (darwin-aarch64), with a signature.
    has_platform: bool,
}

/// An update manifest in the shape tauri-plugin-updater reads: version,
/// notes, pub_date and platforms.<os>-<arch>.{url, signature}.
fn parse_manifest(json: &str) -> Option<Manifest> {
    let v: serde_json::Value = serde_json::from_str(json).ok()?;
    let version = v.get("version")?.as_str()?.trim().trim_start_matches('v').to_string();
    parse_version(&version)?;
    let text = |k: &str| v.get(k).and_then(|x| x.as_str()).map(str::trim).filter(|s| !s.is_empty()).map(str::to_string);
    let has_platform = v.get("platforms").and_then(|p| p.get(PLATFORM)).is_some_and(|p| {
        p.get("url").and_then(|u| u.as_str()).is_some_and(|u| u.starts_with("https://"))
            && p.get("signature").and_then(|s| s.as_str()).is_some_and(|s| !s.trim().is_empty())
    });
    Some(Manifest { version, notes: text("notes").map(|n| n.chars().take(4000).collect()), pub_date: text("pub_date"), has_platform })
}

/// The release page of a version.
fn release_page(version: &str) -> String {
    format!("{RELEASES_PAGE}/tag/v{}", version.trim_start_matches('v'))
}

/// The first endpoint's manifest. Ok(None): none published (404 everywhere,
/// or no endpoint at all).
fn fetch_latest() -> Result<Option<Manifest>, String> {
    let mut last_err = None;
    for url in ENDPOINTS {
        let out = Command::new("/usr/bin/curl")
            .args(["-sSL", "--max-time", "10", "-H", "Accept: application/json", "-A", "folklore-launcher", "-w", "\n%{http_code}", url])
            .stdin(Stdio::null())
            .output()
            .map_err(|e| format!("curl: {e}"))?;
        let text = String::from_utf8_lossy(&out.stdout);
        let (body, code) = text.rsplit_once('\n').unwrap_or(("", text.as_ref()));
        match code.trim() {
            "200" => match parse_manifest(body) {
                Some(m) => return Ok(Some(m)),
                None => last_err = Some("unexpected answer from GitHub".to_string()),
            },
            "404" => {}
            "000" | "" => last_err = Some(String::from_utf8_lossy(&out.stderr).trim().to_string()),
            c => last_err = Some(format!("GitHub answered HTTP {c}")),
        }
    }
    match last_err {
        Some(e) => Err(e),
        None => Ok(None),
    }
}

/// `FOLKLORE_NO_UPDATE_CHECK` set to anything but empty or 0.
pub fn disabled() -> bool {
    std::env::var_os("FOLKLORE_NO_UPDATE_CHECK").is_some_and(|v| !v.is_empty() && v != "0")
}

/// Why the bundle at `bundle` cannot be replaced in place (see AppUpdate).
fn blocker_for(bundle: Option<&Path>) -> Option<&'static str> {
    if ENDPOINTS.is_empty() {
        return Some("nofeed");
    }
    if !cfg!(all(target_os = "macos", target_arch = "aarch64")) {
        return Some("platform");
    }
    let Some(b) = bundle else { return Some("dev") };
    if b.to_string_lossy().contains("/AppTranslocation/") {
        Some("translocated")
    } else if b.starts_with("/Volumes") && !writable_dir(b.parent()) {
        Some("dmg")
    } else {
        None
    }
}

/// Whether a file can be made in `dir` (not a mounted disk image).
fn writable_dir(dir: Option<&Path>) -> bool {
    let Some(dir) = dir else { return false };
    let probe = dir.join(format!(".folklore-write-test-{}", std::process::id()));
    match fs::write(&probe, b"") {
        Ok(()) => {
            let _ = fs::remove_file(&probe);
            true
        }
        Err(_) => false,
    }
}

fn blocker() -> Option<&'static str> {
    blocker_for(crate::app_icon::app_bundle().as_deref())
}

/// The record of an install (`app-updated.json`).
#[derive(Serialize, Deserialize, Debug, PartialEq)]
struct Updated {
    from: String,
    to: String,
    /// Seconds since the epoch.
    at: u64,
}

fn marker_path() -> PathBuf {
    crate::data_root().join(MARKER)
}

/// The version this app was updated from, when the build is older than that
/// update (`built_at` 0: no build, nothing to rebuild).
fn updated_from(marker: Option<&str>, current: &str, built_at: u64) -> Option<String> {
    let u: Updated = serde_json::from_str(marker?).ok()?;
    (u.to.trim_start_matches('v') == current.trim_start_matches('v') && built_at > 0 && built_at < u.at).then_some(u.from)
}

/// The launcher's side of check_updates (`online`: ask the manifest).
pub fn check_app(app: &AppHandle, online: bool, built_at: u64) -> AppUpdate {
    let current = app.package_info().version.to_string();
    let marker = fs::read_to_string(marker_path()).ok();
    let mut u = AppUpdate {
        feed: !ENDPOINTS.is_empty(),
        updated_from: updated_from(marker.as_deref(), &current, built_at),
        installed_pending: app.state::<crate::AppState>().update_installed.lock().unwrap().clone(),
        install_blocker: blocker().map(str::to_string),
        disabled: disabled(),
        current,
        ..Default::default()
    };
    if !online || u.disabled || !u.feed {
        return u;
    }
    u.checked = true;
    match fetch_latest() {
        Ok(Some(m)) => {
            u.newer = is_newer(&m.version, &u.current);
            if !m.has_platform && u.install_blocker.is_none() {
                u.install_blocker = Some("platform".into());
            }
            u.url = Some(release_page(&m.version));
            u.latest = Some(m.version);
            u.notes = m.notes;
            u.pub_date = m.pub_date;
        }
        Ok(None) => u.error = Some("no release with an update manifest (latest.json) yet".into()),
        Err(e) => u.error = Some(e),
    }
    u
}

/// "Download and install". There is no release feed for this launcher yet,
/// so this refuses (the window hides the button; the refusal is for a stale
/// window). With a feed it would download, verify and swap the bundle in
/// with tauri-plugin-updater, record `app-updated.json` and set
/// AppState::update_installed for "Restart to finish".
#[tauri::command]
pub async fn install_update(app: AppHandle) -> Result<String, String> {
    if let Some(b) = blocker() {
        return Err(format!("blocked:{b}"));
    }
    let st = app.state::<crate::AppState>();
    if crate::game_busy(&st) {
        return Err("blocked:game".into());
    }
    if st.rebuild.lock().unwrap().is_some() {
        return Err("blocked:rebuild".into());
    }
    Err("blocked:nofeed".into())
}

/// "Restart to finish": relaunch into the installed version.
#[tauri::command]
pub fn restart_app(app: AppHandle) -> Result<(), String> {
    let st = app.state::<crate::AppState>();
    if st.game.lock().unwrap().is_some() {
        return Err("blocked:game".into());
    }
    if st.rebuild.lock().unwrap().is_some() {
        return Err("blocked:rebuild".into());
    }
    app.restart()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn versions() {
        assert_eq!(parse_version("v0.2.10"), Some((0, 2, 10)));
        assert_eq!(parse_version("1.0"), Some((1, 0, 0)));
        assert_eq!(parse_version("0.3.0-beta.1"), Some((0, 3, 0)));
        assert_eq!(parse_version("x"), None);
        assert_eq!(parse_version("1.2.3.4"), None);
        assert!(is_newer("v0.3.0", "0.2.2"));
        assert!(!is_newer("v0.2.2", "0.2.2"));
        assert!(is_newer("0.3.0", "0.3.0-rc1"));
        assert!(!is_newer("0.3.0-rc1", "0.3.0"));
    }

    #[test]
    fn not_a_checkout() {
        let d = std::env::temp_dir().join(format!("fl-nogit-{}", std::process::id()));
        std::fs::create_dir_all(&d).unwrap();
        let s = sources(&d, false);
        assert!(!s.git);
        std::fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    fn manifest() {
        let m = parse_manifest(r#"{"version":"0.2.0","notes":"n","platforms":{"darwin-aarch64":{"url":"https://x/y","signature":"s"}}}"#).unwrap();
        assert_eq!((m.version.as_str(), m.has_platform, m.notes.as_deref()), ("0.2.0", true, Some("n")));
        assert_eq!(release_page(&m.version), "https://github.com/erikomis/folklore-decomp/releases/tag/v0.2.0");
        assert!(release_page(&m.version).starts_with(crate::URL_ALLOWED[1]));
        assert_eq!(parse_manifest(r#"{"message":"Not Found"}"#), None);
    }

    #[test]
    fn no_feed_yet() {
        // no release feed: nothing is fetched, the install is refused
        assert!(ENDPOINTS.is_empty());
        assert_eq!(fetch_latest(), Ok(None));
        assert_eq!(blocker_for(None), Some("nofeed"));
    }

    #[test]
    fn rebuild_offered_after_an_update() {
        let rec = |to: &str, at: u64| serde_json::to_string(&Updated { from: "0.1.0".into(), to: to.into(), at }).unwrap();
        assert_eq!(updated_from(Some(&rec("0.2.0", 2000)), "0.2.0", 1000).as_deref(), Some("0.1.0"));
        assert_eq!(updated_from(Some(&rec("0.2.0", 2000)), "0.2.0", 2500), None);
        assert_eq!(updated_from(Some(&rec("0.2.0", 2000)), "0.2.0", 0), None);
        assert_eq!(updated_from(None, "0.2.0", 1000), None);
    }
}
