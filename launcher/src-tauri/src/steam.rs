//! "Add to Steam": Folklore as a non-Steam game in the player's Steam
//! library, so it starts from Big Picture with the controller. Optional:
//! nothing here runs unless the player presses the button.
//!
//! - The entry goes into every Steam account's
//!   `userdata/<id>/config/shortcuts.vdf` (binary VDF, vdf.rs). Entries made
//!   by Steam or other tools are kept byte for byte; ours is recognised by
//!   its `Exe` or its app id. The file is copied to
//!   `shortcuts.vdf.folklore-backup` before every write and replaced
//!   atomically.
//! - Its executable is the Play shortcut's Steam kind (play_shortcut.rs),
//!   kept in the launcher's data folder: it opens the launcher with `--play`,
//!   which starts the game, and waits until the launcher closes (it does when
//!   the game ends), so Steam sees the game running exactly while it is.
//! - The app id is Steam's own for non-Steam games: CRC32 of `Exe` followed by
//!   `AppName`, with the top bit set. The library art is named after it in
//!   `config/grid/` (steam_art.rs draws it from the player's copy).
//! - Steam rewrites shortcuts.vdf from memory when it quits, so nothing is
//!   written while it runs (the window offers to quit it first).

use crate::vdf::{self, List, Value};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

pub const APP_NAME: &str = "Folklore";
pub const FILE: &str = "shortcuts.vdf";
pub const BACKUP: &str = "shortcuts.vdf.folklore-backup";

/// Steam's folder (macOS; the game only runs there for now).
pub fn root(home: &Path) -> PathBuf {
    home.join("Library/Application Support/Steam")
}

/// A Steam account on this computer: `userdata/<id>/config`.
#[derive(Debug, Clone, PartialEq)]
pub struct User {
    pub id: String,
    pub config: PathBuf,
}

/// The accounts that have signed in here (numeric folders with a `config/`;
/// `0` is Steam's anonymous placeholder).
pub fn users(root: &Path) -> Vec<User> {
    let Ok(rd) = fs::read_dir(root.join("userdata")) else { return Vec::new() };
    let mut v: Vec<User> = rd
        .flatten()
        .filter_map(|e| {
            let id = e.file_name().to_string_lossy().into_owned();
            let config = e.path().join("config");
            (!id.is_empty() && id != "0" && id.bytes().all(|c| c.is_ascii_digit()) && config.is_dir())
                .then_some(User { id, config })
        })
        .collect();
    v.sort_by(|a, b| a.id.cmp(&b.id));
    v
}

/// Whether the Steam client is running (`steam_osx` on macOS).
pub fn running() -> bool {
    cfg!(target_os = "macos")
        && Command::new("pgrep")
            .args(["-x", "steam_osx"])
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .is_ok_and(|s| s.success())
}

/// Ask Steam to quit (as Steam > Quit would; it saves its files first).
pub fn quit() -> Result<(), String> {
    let ok = Command::new("/usr/bin/osascript")
        .args(["-e", "tell application \"Steam\" to quit"])
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map_err(|e| e.to_string())?
        .success();
    if ok {
        Ok(())
    } else {
        Err("Steam did not answer".into())
    }
}

/// A path the way Steam stores `Exe` and `StartDir`: in double quotes.
pub fn quoted(p: &Path) -> String {
    format!("\"{}\"", p.display())
}

/// Steam's id for a non-Steam game: CRC32 of the quoted exe and the name,
/// top bit set. The same number names its art in `config/grid/`.
pub fn app_id(exe: &str, name: &str) -> u32 {
    let mut h = crc32fast::Hasher::new();
    h.update(exe.as_bytes());
    h.update(name.as_bytes());
    h.finalize() | 0x8000_0000
}

/// The art files of an id, in `config/grid/`: portrait, landscape, hero, icon.
pub fn art_names(app_id: u32) -> [String; 4] {
    [format!("{app_id}p.png"), format!("{app_id}.png"), format!("{app_id}_hero.png"), format!("{app_id}_icon.png")]
}

/// What the entry points at.
#[derive(Debug, Clone)]
pub struct Target {
    /// The executable, quoted.
    pub exe: String,
    /// Its folder, quoted.
    pub start_dir: String,
}

impl Target {
    pub fn new(exe: &Path) -> Self {
        let dir = exe.parent().unwrap_or(Path::new("/"));
        Target { exe: quoted(exe), start_dir: quoted(dir) }
    }

    pub fn id(&self) -> u32 {
        app_id(&self.exe, APP_NAME)
    }
}

fn is_ours(e: &Value, t: &Target) -> bool {
    let Value::List(e) = e else { return false };
    e.get_str("Exe").as_deref() == Some(t.exe.as_str()) || e.get_int("appid") == Some(t.id() as i32)
}

/// The entry's fields. An entry of ours already there keeps what the player
/// or Steam put in it (play time, tags, hidden, ...); only ours change.
fn entry(t: &Target, icon: &str, old: Option<List>) -> List {
    let fresh = old.is_none();
    let mut e = old.unwrap_or_default();
    e.set("appid", Value::Int(t.id() as i32));
    e.set("AppName", vdf::str_value(APP_NAME));
    e.set("Exe", vdf::str_value(&t.exe));
    e.set("StartDir", vdf::str_value(&t.start_dir));
    e.set("icon", vdf::str_value(icon));
    if fresh {
        for (k, v) in [
            ("ShortcutPath", vdf::str_value("")),
            ("LaunchOptions", vdf::str_value("")),
            ("IsHidden", Value::Int(0)),
            ("AllowDesktopConfig", Value::Int(1)),
            ("AllowOverlay", Value::Int(1)),
            ("OpenVR", Value::Int(0)),
            ("Devkit", Value::Int(0)),
            ("DevkitGameID", vdf::str_value("")),
            ("DevkitOverrideAppID", Value::Int(0)),
            ("LastPlayTime", Value::Int(0)),
            ("FlatpakAppID", vdf::str_value("")),
            ("tags", Value::List(List::default())),
        ] {
            e.set(k, v);
        }
    }
    e
}

/// The file's root and its `shortcuts` list (made when the file is empty or
/// missing).
fn open(bytes: &[u8]) -> Result<List, String> {
    let mut root = if bytes.is_empty() { List { items: Vec::new(), end: 0xFF } } else { vdf::parse(bytes)? };
    if root.get_list("shortcuts").is_none() {
        if root.get("shortcuts").is_some() {
            return Err("shortcuts.vdf: \"shortcuts\" is not a list".into());
        }
        root.items.push((b"shortcuts".to_vec(), Value::List(List::default())));
        if root.end == 0xFF {
            root.end = 0x08;
        }
    }
    Ok(root)
}

/// Number the entries 0, 1, 2, ... again (Steam's own layout) after one was
/// taken out. Only the keys change.
fn renumber(l: &mut List) {
    for (i, (k, _)) in l.items.iter_mut().enumerate() {
        *k = i.to_string().into_bytes();
    }
}

/// shortcuts.vdf with our entry added (or brought up to date).
pub fn with_entry(bytes: &[u8], t: &Target, icon: &str) -> Result<Vec<u8>, String> {
    let mut root = open(bytes)?;
    let list = root.get_list_mut("shortcuts").unwrap();
    let mine: Vec<usize> = (0..list.items.len()).filter(|&i| is_ours(&list.items[i].1, t)).collect();
    match mine.first() {
        Some(&i) => {
            let old = match &list.items[i].1 {
                Value::List(l) => l.clone(),
                _ => unreachable!(),
            };
            list.items[i].1 = Value::List(entry(t, icon, Some(old)));
            // a second copy of ours (from an older Steam merge) goes
            if mine.len() > 1 {
                for &j in mine[1..].iter().rev() {
                    list.items.remove(j);
                }
                renumber(list);
            }
        }
        None => {
            // the next free number (Steam keeps them 0..n-1)
            let next = list
                .items
                .iter()
                .filter_map(|(k, _)| std::str::from_utf8(k).ok()?.parse::<u64>().ok())
                .max()
                .map_or(0, |m| m + 1)
                .max(list.items.len() as u64);
            list.items.push((next.to_string().into_bytes(), Value::List(entry(t, icon, None))));
        }
    }
    Ok(vdf::write(&root))
}

/// shortcuts.vdf without our entry; None when it was not there.
pub fn without_entry(bytes: &[u8], t: &Target) -> Result<Option<Vec<u8>>, String> {
    if bytes.is_empty() {
        return Ok(None);
    }
    let mut root = vdf::parse(bytes)?;
    let Some(list) = root.get_list_mut("shortcuts") else { return Ok(None) };
    let before = list.items.len();
    list.items.retain(|(_, v)| !is_ours(v, t));
    if list.items.len() == before {
        return Ok(None);
    }
    renumber(list);
    Ok(Some(vdf::write(&root)))
}

pub fn has_entry(bytes: &[u8], t: &Target) -> bool {
    vdf::parse(bytes)
        .ok()
        .and_then(|r| r.get_list("shortcuts").map(|l| l.items.iter().any(|(_, v)| is_ours(v, t))))
        .unwrap_or(false)
}

fn read(config: &Path) -> Result<Vec<u8>, String> {
    let p = config.join(FILE);
    match fs::read(&p) {
        Ok(b) => Ok(b),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Vec::new()),
        Err(e) => Err(format!("{}: {e}", p.display())),
    }
}

/// Write shortcuts.vdf: the old one copied to the backup first, the new one
/// through a temporary file and a rename (never half written).
fn replace(config: &Path, bytes: &[u8]) -> Result<(), String> {
    let p = config.join(FILE);
    if p.is_file() {
        fs::copy(&p, config.join(BACKUP)).map_err(|e| format!("{}: {e}", config.join(BACKUP).display()))?;
    }
    let tmp = config.join("shortcuts.vdf.folklore-tmp");
    fs::write(&tmp, bytes).map_err(|e| format!("{}: {e}", tmp.display()))?;
    fs::rename(&tmp, &p).map_err(|e| format!("{}: {e}", p.display()))
}

/// Whether this account has our entry.
pub fn user_has(u: &User, t: &Target) -> bool {
    read(&u.config).is_ok_and(|b| has_entry(&b, t))
}

/// The art as PNG files, by name suffix (art_names order).
pub type Art = [Vec<u8>; 4];

/// Add (or update) the entry for one account, and its art when there is some.
pub fn add(u: &User, t: &Target, art: Option<&Art>) -> Result<(), String> {
    let grid = u.config.join("grid");
    let names = art_names(t.id());
    let icon = match art {
        Some(_) => grid.join(&names[3]).to_string_lossy().into_owned(),
        None => String::new(),
    };
    let old = read(&u.config)?;
    let new = with_entry(&old, t, &icon)?;
    if let Some(art) = art {
        fs::create_dir_all(&grid).map_err(|e| format!("{}: {e}", grid.display()))?;
        for (n, png) in names.iter().zip(art.iter()) {
            let p = grid.join(n);
            let tmp = grid.join(format!("{n}.tmp"));
            fs::write(&tmp, png).and_then(|_| fs::rename(&tmp, &p)).map_err(|e| format!("{}: {e}", p.display()))?;
        }
    }
    if new != old {
        replace(&u.config, &new)?;
    }
    Ok(())
}

/// Take the entry and the art it made out of one account. Returns whether
/// there was an entry.
pub fn remove(u: &User, t: &Target) -> Result<bool, String> {
    let old = read(&u.config)?;
    let found = match without_entry(&old, t)? {
        Some(new) => {
            replace(&u.config, &new)?;
            true
        }
        None => false,
    };
    for n in art_names(t.id()) {
        let p = u.config.join("grid").join(n);
        if p.is_file() {
            fs::remove_file(&p).map_err(|e| format!("{}: {e}", p.display()))?;
        }
    }
    Ok(found)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("i2-steam-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    fn target() -> Target {
        Target::new(Path::new("/Users/p/Library/Application Support/folklore-recomp/steam/Play Folklore.app/Contents/MacOS/play"))
    }

    #[test]
    fn ids_as_steam_computes_them() {
        // CRC32("\"/a/b\"" + "Game") = 0x8cbe8c22 (Python zlib.crc32); the top bit is set anyway
        assert_eq!(app_id("\"/a/b\"", "Game"), 0x8cbe8c22);
        assert_eq!(app_id("x", ""), crc32fast::hash(b"x") | 0x8000_0000);
        let id = target().id();
        assert!(id & 0x8000_0000 != 0);
        assert_eq!(art_names(123)[0], "123p.png");
        assert_eq!(art_names(123)[1], "123.png");
        assert_eq!(art_names(123)[2], "123_hero.png");
        assert_eq!(art_names(123)[3], "123_icon.png");
        let t = target();
        assert_eq!(t.start_dir, "\"/Users/p/Library/Application Support/folklore-recomp/steam/Play Folklore.app/Contents/MacOS\"");
        assert!(t.exe.starts_with('"') && t.exe.ends_with("/play\""));
    }

    #[test]
    fn adds_next_to_other_entries_and_takes_only_ours_out() {
        let orig = crate::vdf::tests::sample();
        let t = target();
        let added = with_entry(&orig, &t, "/grid/x_icon.png").unwrap();
        let root = vdf::parse(&added).unwrap();
        let list = root.get_list("shortcuts").unwrap();
        assert_eq!(list.items.len(), 3);
        assert_eq!(list.items[2].0, b"2");
        let e = list.get_list("2").unwrap();
        assert_eq!(e.get_str("AppName").as_deref(), Some(APP_NAME));
        assert_eq!(e.get_str("Exe").as_deref(), Some(t.exe.as_str()));
        assert_eq!(e.get_int("appid"), Some(t.id() as i32));
        assert_eq!(e.get_str("icon").as_deref(), Some("/grid/x_icon.png"));
        // the other two are untouched: the file starts with exactly their bytes
        let orig_root = vdf::parse(&orig).unwrap();
        assert_eq!(list.items[..2], orig_root.get_list("shortcuts").unwrap().items[..]);
        assert!(has_entry(&added, &t));
        assert!(!has_entry(&orig, &t));
        // adding again changes nothing but our own fields
        assert_eq!(with_entry(&added, &t, "/grid/x_icon.png").unwrap(), added);
        // out again: byte for byte the file we started from
        assert_eq!(without_entry(&added, &t).unwrap(), Some(orig.clone()));
        assert_eq!(without_entry(&orig, &t).unwrap(), None);
    }

    #[test]
    fn keeps_the_players_changes_to_our_entry() {
        let t = target();
        let added = with_entry(b"\0shortcuts\0\x08\x08", &t, "").unwrap();
        let mut root = vdf::parse(&added).unwrap();
        let e = root.get_list_mut("shortcuts").unwrap().get_list_mut("0").unwrap();
        e.set("LastPlayTime", Value::Int(1_800_000_000));
        e.set("IsHidden", Value::Int(1));
        e.set("FutureField", vdf::str_value("x"));
        let played = vdf::write(&root);
        let again = vdf::parse(&with_entry(&played, &t, "/new_icon.png").unwrap()).unwrap();
        let e = again.get_list("shortcuts").unwrap().get_list("0").unwrap();
        assert_eq!(e.get_int("LastPlayTime"), Some(1_800_000_000));
        assert_eq!(e.get_int("IsHidden"), Some(1));
        assert_eq!(e.get_str("FutureField").as_deref(), Some("x"));
        assert_eq!(e.get_str("icon").as_deref(), Some("/new_icon.png"));
    }

    #[test]
    fn removing_from_the_middle_renumbers_the_rest() {
        let t = target();
        // ours first, then the sample's two
        let empty = b"\0shortcuts\0\x08\x08";
        let ours = vdf::parse(&with_entry(empty, &t, "").unwrap()).unwrap().get_list("shortcuts").unwrap().items[0].1.clone();
        let mut root = vdf::parse(&crate::vdf::tests::sample()).unwrap();
        let list = root.get_list_mut("shortcuts").unwrap();
        list.items.insert(0, (b"0".to_vec(), ours));
        renumber(list);
        let before = vdf::write(&root);
        let after = vdf::parse(&without_entry(&before, &t).unwrap().unwrap()).unwrap();
        let l = after.get_list("shortcuts").unwrap();
        assert_eq!(l.items.iter().map(|(k, _)| k.clone()).collect::<Vec<_>>(), vec![b"0".to_vec(), b"1".to_vec()]);
        assert_eq!(l.get_list("0").unwrap().get_str("Exe").as_deref(), Some("\"/Applications/Emu.app\""));
    }

    #[test]
    fn empty_or_missing_file_and_duplicates() {
        let t = target();
        let made = with_entry(b"", &t, "").unwrap();
        assert!(made.starts_with(b"\0shortcuts\0\x000\0"));
        assert!(made.ends_with(b"\x08\x08\x08"));
        // two copies of ours (an old merge): one stays
        let mut root = vdf::parse(&made).unwrap();
        let list = root.get_list_mut("shortcuts").unwrap();
        let dup = list.items[0].clone();
        list.items.push((b"1".to_vec(), dup.1));
        let fixed = vdf::parse(&with_entry(&vdf::write(&root), &t, "").unwrap()).unwrap();
        assert_eq!(fixed.get_list("shortcuts").unwrap().items.len(), 1);
        assert!(with_entry(b"\x01shortcuts\0x\0\x08", &t, "").is_err());
        assert!(with_entry(b"garbage", &t, "").is_err());
    }

    #[test]
    fn accounts_backup_and_art_on_disk() {
        let d = tmp("users");
        let cfg = d.join("userdata/12345/config");
        fs::create_dir_all(&cfg).unwrap();
        fs::create_dir_all(d.join("userdata/0/config")).unwrap();
        fs::create_dir_all(d.join("userdata/ac")).unwrap();
        fs::create_dir_all(d.join("userdata/777")).unwrap(); // no config yet
        let us = users(&d);
        assert_eq!(us, vec![User { id: "12345".into(), config: cfg.clone() }]);
        let u = &us[0];
        let orig = crate::vdf::tests::sample();
        fs::write(cfg.join(FILE), &orig).unwrap();
        let t = target();
        let art: Art = [b"p".to_vec(), b"l".to_vec(), b"h".to_vec(), b"i".to_vec()];
        add(u, &t, Some(&art)).unwrap();
        assert_eq!(fs::read(cfg.join(BACKUP)).unwrap(), orig);
        assert!(user_has(u, &t));
        let names = art_names(t.id());
        assert_eq!(fs::read(cfg.join("grid").join(&names[0])).unwrap(), b"p");
        assert_eq!(fs::read(cfg.join("grid").join(&names[3])).unwrap(), b"i");
        let e = vdf::parse(&fs::read(cfg.join(FILE)).unwrap()).unwrap();
        let icon = e.get_list("shortcuts").unwrap().get_list("2").unwrap().get_str("icon").unwrap();
        assert_eq!(PathBuf::from(icon), cfg.join("grid").join(&names[3]));
        // someone else's art in grid/ stays
        fs::write(cfg.join("grid/999p.png"), b"x").unwrap();
        assert!(remove(u, &t).unwrap());
        assert_eq!(fs::read(cfg.join(FILE)).unwrap(), orig);
        assert!(!user_has(u, &t));
        for n in &names {
            assert!(!cfg.join("grid").join(n).exists());
        }
        assert!(cfg.join("grid/999p.png").exists());
        assert!(!remove(u, &t).unwrap());
        // no shortcuts.vdf at all: made
        let cfg2 = d.join("userdata/888/config");
        fs::create_dir_all(&cfg2).unwrap();
        let u2 = User { id: "888".into(), config: cfg2.clone() };
        add(&u2, &t, None).unwrap();
        assert!(user_has(&u2, &t));
        assert!(!cfg2.join(BACKUP).exists());
        let _ = fs::remove_dir_all(&d);
    }

    /// On a copy of a real shortcuts.vdf: FOLKLORE_VDF=<copy> cargo test real_file -- --ignored
    #[test]
    #[ignore]
    fn real_file() {
        let p = PathBuf::from(std::env::var("FOLKLORE_VDF").expect("FOLKLORE_VDF"));
        let orig = fs::read(&p).unwrap();
        assert_eq!(vdf::write(&vdf::parse(&orig).unwrap()), orig, "round trip");
        let t = target();
        let added = with_entry(&orig, &t, "/x_icon.png").unwrap();
        fs::write(p.with_extension("added.vdf"), &added).unwrap();
        assert!(has_entry(&added, &t));
        assert_eq!(without_entry(&added, &t).unwrap().as_deref(), Some(&orig[..]));
    }
}
