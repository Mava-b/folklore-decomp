//! The "Play" shortcut: `--play` on the launcher's command line starts the
//! game as soon as the launcher is up, with the saved settings (the same Play
//! as the button: the save backup, the settings, the log), and a tiny .app on
//! the Desktop runs `open -b <launcher id> --args --play`, so the player
//! double-clicks one icon instead of opening the launcher and pressing Play.
//!
//! The shortcut is a shell-script bundle the launcher writes on request (Home
//! > Status > Play shortcut on the Desktop). It finds the launcher through
//! LaunchServices by its bundle id, so it keeps working when the app moves.
//! Its icon is the launcher's own placeholder `.icns` (never game art); the
//! player's ICON0 square goes on it as a Finder icon only at runtime, like
//! the launcher's own "Use as the app's Finder icon".
//!
//! "Add to Steam" (steam.rs) reuses it: a second copy of the bundle in the
//! launcher's data folder (`Kind::Steam`) is the non-Steam game's executable.
//! Its script waits (`open -W`) until the launcher quits and passes
//! `--exit-with-game`, which closes the launcher once the game ends normally,
//! so Steam sees the game running exactly as long as it is and Big Picture
//! comes back by itself.

use std::fs;
use std::path::{Path, PathBuf};

/// The argument that asks for the game straight away.
pub const ARG: &str = "--play";

/// With ARG: quit the launcher once that game has ended normally (the Steam
/// shortcut).
pub const EXIT_ARG: &str = "--exit-with-game";

/// Which shortcut: the one on the Desktop, or the Steam entry's executable.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Kind {
    Desktop,
    Steam,
}

/// The shortcut's file name on the Desktop.
pub const NAME: &str = "Play Folklore.app";

/// Whether the command line (argv, program first) asks for the game at once.
pub fn requested<I: IntoIterator<Item = String>>(args: I) -> bool {
    args.into_iter().skip(1).any(|a| a == ARG)
}

/// Whether the command line asks to quit after the game (only with ARG).
pub fn exit_requested(args: &[String]) -> bool {
    requested(args.iter().cloned()) && args.iter().skip(1).any(|a| a == EXIT_ARG)
}

/// `--view <page>` or `--view=<page>`: the page the window opens on (for
/// docs screenshots and a shortcut to a page; the window checks the name).
pub fn start_view(args: &[String]) -> Option<String> {
    let mut it = args.iter().skip(1);
    while let Some(a) = it.next() {
        if let Some(v) = a.strip_prefix("--view=") {
            return Some(v.to_string());
        }
        if a == "--view" {
            return it.next().cloned();
        }
    }
    None
}

/// The shortcut's own bundle id: the launcher's plus `.play` (`.steam` for
/// the Steam one, so the two never pass for each other).
pub fn shortcut_id(launcher_id: &str) -> String {
    format!("{launcher_id}.play")
}

fn kind_id(launcher_id: &str, kind: Kind) -> String {
    match kind {
        Kind::Desktop => shortcut_id(launcher_id),
        Kind::Steam => format!("{launcher_id}.steam"),
    }
}

fn sh_quote(v: &str) -> String {
    format!("'{}'", v.replace('\'', r"'\''"))
}

fn xml_escape(v: &str) -> String {
    v.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;")
}

/// The bundle's executable.
pub fn script_for(launcher_id: &str, kind: Kind) -> String {
    match kind {
        Kind::Desktop => format!(
            "#!/bin/sh\n\
             # Made by the Folklore launcher (Home > Status > Play shortcut).\n\
             # Opens the launcher, which starts the game with your saved settings.\n\
             # If the launcher is already open it only comes to the front.\n\
             exec /usr/bin/open -b {} --args {ARG}\n",
            sh_quote(launcher_id)
        ),
        Kind::Steam => format!(
            "#!/bin/sh\n\
             # Made by the Folklore launcher (Home > Status > Add to Steam).\n\
             # Steam runs this: it opens the launcher, which starts the game with your\n\
             # saved settings and closes when the game ends; this waits for that, so\n\
             # Steam sees the game running as long as it is.\n\
             exec /usr/bin/open -W -b {} --args {ARG} {EXIT_ARG}\n",
            sh_quote(launcher_id)
        ),
    }
}

/// The bundle's Info.plist. `icon`: the icon file's name in Resources.
fn plist_for(launcher_id: &str, icon: Option<&str>, kind: Kind) -> String {
    let icon = icon
        .map(|i| format!("  <key>CFBundleIconFile</key>\n  <string>{}</string>\n", xml_escape(i)))
        .unwrap_or_default();
    format!(
        r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key>
  <string>play</string>
  <key>CFBundleIdentifier</key>
  <string>{}</string>
  <key>CFBundleName</key>
  <string>Play Folklore</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleInfoDictionaryVersion</key>
  <string>6.0</string>
  <key>CFBundleShortVersionString</key>
  <string>1.0</string>
{icon}  <key>LSUIElement</key>
  <true/>
</dict>
</plist>
"#,
        xml_escape(&kind_id(launcher_id, kind))
    )
}

/// The launcher bundle's own icon (`Contents/Resources/*.icns`; Tauri names
/// it `icon.icns`).
pub fn bundle_icns(bundle: &Path) -> Option<PathBuf> {
    let res = bundle.join("Contents/Resources");
    let preferred = res.join("icon.icns");
    if preferred.is_file() {
        return Some(preferred);
    }
    let mut found: Vec<PathBuf> = fs::read_dir(&res)
        .ok()?
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.extension().is_some_and(|e| e == "icns") && p.is_file())
        .collect();
    found.sort();
    found.into_iter().next()
}

/// Whether `path` is a shortcut of this kind this module wrote (safe to replace).
pub fn is_ours(path: &Path, launcher_id: &str, kind: Kind) -> bool {
    fs::read_to_string(path.join("Contents/Info.plist"))
        .is_ok_and(|p| p.contains(&format!("<string>{}</string>", xml_escape(&kind_id(launcher_id, kind)))))
}

/// The bundle's executable inside it.
pub fn executable(bundle: &Path) -> PathBuf {
    bundle.join("Contents/MacOS/play")
}

/// Write the shortcut into `dir` (an older one of ours is replaced; anything
/// else with that name is left alone). Returns its path.
pub fn write(dir: &Path, launcher_id: &str, icns: Option<&Path>) -> Result<PathBuf, String> {
    write_kind(dir, launcher_id, icns, Kind::Desktop)
}

pub fn write_kind(dir: &Path, launcher_id: &str, icns: Option<&Path>, kind: Kind) -> Result<PathBuf, String> {
    let path = dir.join(NAME);
    if path.exists() || path.symlink_metadata().is_ok() {
        if !is_ours(&path, launcher_id, kind) {
            return Err(format!("{}: something else already has that name", path.display()));
        }
        fs::remove_dir_all(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    }
    let contents = path.join("Contents");
    let macos = contents.join("MacOS");
    fs::create_dir_all(&macos).map_err(|e| format!("{}: {e}", macos.display()))?;
    let icon_name = match icns {
        Some(src) => {
            let res = contents.join("Resources");
            fs::create_dir_all(&res).map_err(|e| format!("{}: {e}", res.display()))?;
            fs::copy(src, res.join("AppIcon.icns")).map_err(|e| format!("{}: {e}", src.display()))?;
            Some("AppIcon")
        }
        None => None,
    };
    fs::write(contents.join("Info.plist"), plist_for(launcher_id, icon_name, kind)).map_err(|e| e.to_string())?;
    let exe = executable(&path);
    fs::write(&exe, script_for(launcher_id, kind)).map_err(|e| e.to_string())?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(&exe, fs::Permissions::from_mode(0o755)).map_err(|e| e.to_string())?;
    }
    Ok(path)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn start_view_argument() {
        let a = |v: &[&str]| v.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        assert_eq!(start_view(&a(&["app", "--view", "controls"])).as_deref(), Some("controls"));
        assert_eq!(start_view(&a(&["app", "--play", "--view=graphics"])).as_deref(), Some("graphics"));
        assert_eq!(start_view(&a(&["app", "--view"])), None);
        assert_eq!(start_view(&a(&["app", "--play"])), None);
    }

    const ID: &str = "io.github.erikomis.folklorerecomp";

    fn tmp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("i2-shortcut-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn play_argument() {
        let a = |v: &[&str]| requested(v.iter().map(|s| s.to_string()));
        assert!(a(&["launcher", "--play"]));
        assert!(a(&["launcher", "-x", "--play"]));
        assert!(!a(&["launcher"]));
        assert!(!a(&["--play"])); // the program name is not an argument
        assert!(!a(&["launcher", "--player"]));
    }

    #[test]
    fn script_opens_the_launcher_by_id() {
        let s = script_for(ID, Kind::Desktop);
        assert!(s.starts_with("#!/bin/sh\n"));
        assert!(s.contains("exec /usr/bin/open -b 'io.github.erikomis.folklorerecomp' --args --play\n"));
        assert!(script_for("a'b", Kind::Desktop).contains(r"-b 'a'\''b'"));
    }

    #[test]
    fn writes_a_bundle_and_replaces_its_own() {
        let d = tmp("write");
        let icns = d.join("icon.icns");
        fs::write(&icns, b"icns").unwrap();
        let p = write(&d, ID, Some(&icns)).unwrap();
        assert_eq!(p, d.join(NAME));
        let plist = fs::read_to_string(p.join("Contents/Info.plist")).unwrap();
        assert!(plist.contains("<string>io.github.erikomis.folklorerecomp.play</string>"));
        assert!(plist.contains("<key>CFBundleIconFile</key>\n  <string>AppIcon</string>"));
        assert!(plist.contains("<key>CFBundleExecutable</key>\n  <string>play</string>"));
        assert_eq!(fs::read(p.join("Contents/Resources/AppIcon.icns")).unwrap(), b"icns");
        let exe = p.join("Contents/MacOS/play");
        assert_eq!(fs::read_to_string(&exe).unwrap(), script_for(ID, Kind::Desktop));
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(fs::metadata(&exe).unwrap().permissions().mode() & 0o777, 0o755);
        }
        // again, without an icon: ours, so replaced
        fs::write(p.join("Contents/stale"), b"x").unwrap();
        let p = write(&d, ID, None).unwrap();
        assert!(!p.join("Contents/stale").exists());
        assert!(!p.join("Contents/Resources").exists());
        assert!(!fs::read_to_string(p.join("Contents/Info.plist")).unwrap().contains("CFBundleIconFile"));
        let _ = fs::remove_dir_all(&d);
    }

    #[test]
    fn steam_kind_waits_and_exits_with_the_game() {
        let s = script_for(ID, Kind::Steam);
        assert!(s.contains("exec /usr/bin/open -W -b 'io.github.erikomis.folklorerecomp' --args --play --exit-with-game\n"));
        let d = tmp("steam");
        let p = write_kind(&d, ID, None, Kind::Steam).unwrap();
        assert!(is_ours(&p, ID, Kind::Steam));
        assert!(!is_ours(&p, ID, Kind::Desktop));
        assert!(fs::read_to_string(p.join("Contents/Info.plist")).unwrap().contains("<string>io.github.erikomis.folklorerecomp.steam</string>"));
        assert_eq!(fs::read_to_string(executable(&p)).unwrap(), s);
        // the Desktop kind does not replace the Steam one (and the other way round)
        assert!(write_kind(&d, ID, None, Kind::Desktop).is_err());
        let _ = fs::remove_dir_all(&d);
        let args = |v: &[&str]| v.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        assert!(exit_requested(&args(&["l", "--play", "--exit-with-game"])));
        assert!(!exit_requested(&args(&["l", "--exit-with-game"])));
        assert!(!exit_requested(&args(&["l", "--play"])));
    }

    #[test]
    fn leaves_someone_elses_file_alone() {
        let d = tmp("other");
        fs::create_dir_all(d.join(NAME).join("Contents")).unwrap();
        fs::write(d.join(NAME).join("Contents/Info.plist"), "<string>com.example.other</string>").unwrap();
        assert!(write(&d, ID, None).is_err());
        assert!(d.join(NAME).join("Contents/Info.plist").exists());
        let _ = fs::remove_dir_all(&d);
    }

    #[test]
    fn finds_the_bundle_icon() {
        let d = tmp("icns");
        let res = d.join("A.app/Contents/Resources");
        fs::create_dir_all(&res).unwrap();
        assert_eq!(bundle_icns(&d.join("A.app")), None);
        fs::write(res.join("zz.icns"), b"").unwrap();
        assert_eq!(bundle_icns(&d.join("A.app")), Some(res.join("zz.icns")));
        fs::write(res.join("icon.icns"), b"").unwrap();
        assert_eq!(bundle_icns(&d.join("A.app")), Some(res.join("icon.icns")));
        let _ = fs::remove_dir_all(&d);
    }
}
