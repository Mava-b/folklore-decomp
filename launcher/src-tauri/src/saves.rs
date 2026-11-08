//! The saves manager: Folklore's save slots in the repository folder,
//! timestamped backups, restore, delete and .zip archives.
//!
//! The game saves through cellSaveData: one folder per slot, named after the
//! title ID (`BCUS98147…` for the North American disc, `BCES00050…` for the
//! European one), holding PARAM.SFO, ICON0.PNG and the game's files. On the
//! Mac the toolkit writes them under the working directory run_mac.sh starts
//! the game in (the repository): `gamedata/dev_hdd0/home/00000001/savedata`
//! (PS3_GAMEDATA_ROOT replaces `gamedata`). run_mac.sh also makes
//! `hdd0/home/00000001/savedata` (the PS3_HDD0_ROOT side), so slots found
//! there are listed and backed up too. Each place is a `Root` with a short
//! label; a slot is named `<label>/<folder>` in the window and in backups.
//!
//! Every operation that replaces or removes a save backs it up first, and a
//! slot is swapped in whole (built next to it, then renamed), so a failed
//! copy leaves the old one in place. A slot imported from RPCS3 (no
//! PARAM.PFD, one `*<file>` key per file in PARAM.SFO) gets the port's
//! PARAM.PFD (the SECUREFILE names, one per line) made from those keys.
//!
//! Everything takes its folders from `Locations`, so the tests run on
//! temporary directories.

use crate::zipfile;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

/// The releases the Mac build runs: North America and Europe (disc 1.00).
pub const TITLE_IDS: [&str; 2] = ["BCUS98147", "BCES00050"];
/// The PS3 user the port's cellSaveData writes under.
pub const PORT_USER: &str = "00000001";
const MANIFEST: &str = "backup.json";
const ZIP_MANIFEST: &str = "folklore-saves.json";
const SFO: &str = "PARAM.SFO";
const PFD: &str = "PARAM.PFD";

/// A folder that holds save slots, with the label slots are named by.
#[derive(Clone, Debug)]
pub struct Root {
    pub label: String,
    pub dir: PathBuf,
}

pub struct Locations {
    /// The savedata folders, the main one (where imports go) first.
    pub roots: Vec<Root>,
    /// Where backups go (`<data>/save-backups`).
    pub backups: PathBuf,
}

impl Locations {
    fn root(&self, label: &str) -> Result<&Root, String> {
        self.roots.iter().find(|r| r.label == label).ok_or_else(|| format!("{label:?}: not a save folder"))
    }
    /// The folder of a slot key (`<label>/<folder>`).
    fn slot_dir(&self, key: &str) -> Result<PathBuf, String> {
        let (label, name) = split_key(key)?;
        Ok(self.root(label)?.dir.join(name))
    }
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

/// A save folder of this game, with a name that is safe to join onto a path.
pub fn valid_slot(name: &str) -> bool {
    name.len() <= 64
        && TITLE_IDS.iter().any(|t| name.len() > t.len() && name.starts_with(t))
        && name.chars().all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-')
}

fn valid_label(l: &str) -> bool {
    !l.is_empty() && l.len() <= 32 && l.chars().all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-')
}

/// `<label>/<folder>` -> (label, folder), checked.
fn split_key(key: &str) -> Result<(&str, &str), String> {
    match key.split_once('/') {
        Some((l, n)) if valid_label(l) && valid_slot(n) => Ok((l, n)),
        _ => Err(format!("{key:?}: not a Folklore save")),
    }
}

fn valid_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 128
        && !id.starts_with('.')
        && id.chars().all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-')
}

/// One path component from an archive.
fn safe_component(n: &str) -> bool {
    !n.is_empty() && n != "." && n != ".." && n.len() <= 255 && !n.contains(['/', '\\', '\0', ':'])
}

// ---------------------------------------------------------------------------
// PARAM.SFO
// ---------------------------------------------------------------------------

pub const PSF_INT: u16 = 0x0404;

/// One key of a binary PSF (PARAM.SFO).
#[derive(Clone, Debug, PartialEq)]
pub struct PsfEntry {
    pub key: String,
    pub fmt: u16,
    /// Bytes used (strings include their NUL).
    pub data: Vec<u8>,
}

impl PsfEntry {
    fn as_int(&self) -> Option<u32> {
        (self.fmt == PSF_INT && self.data.len() >= 4).then(|| u32::from_le_bytes(self.data[..4].try_into().unwrap()))
    }
    fn as_string(&self) -> String {
        match self.as_int() {
            Some(n) => n.to_string(),
            None => String::from_utf8_lossy(&self.data).trim_end_matches('\0').to_string(),
        }
    }
}

pub fn psf_parse(d: &[u8]) -> Option<Vec<PsfEntry>> {
    if d.len() < 20 || &d[..4] != b"\0PSF" {
        return None;
    }
    let u32le = |o: usize| -> Option<u32> { Some(u32::from_le_bytes(d.get(o..o + 4)?.try_into().ok()?)) };
    let u16le = |o: usize| -> Option<u16> { Some(u16::from_le_bytes(d.get(o..o + 2)?.try_into().ok()?)) };
    let (kt, dt, n) = (u32le(8)? as usize, u32le(12)? as usize, u32le(16)? as usize);
    let mut out = Vec::new();
    for k in 0..n.min(512) {
        let e = 20 + 16 * k;
        let (ko, fmt, len, off) = (u16le(e)? as usize, u16le(e + 2)?, u32le(e + 4)? as usize, u32le(e + 12)? as usize);
        let key_bytes = d.get(kt + ko..)?;
        let key_end = key_bytes.iter().position(|&b| b == 0)?;
        let key = String::from_utf8_lossy(&key_bytes[..key_end]).into_owned();
        let data = d.get(dt + off..dt + off + len)?.to_vec();
        out.push(PsfEntry { key, fmt, data });
    }
    Some(out)
}

/// PARAM.SFO as key -> value (empty when it is not a PSF).
pub fn sfo_values(d: &[u8]) -> BTreeMap<String, String> {
    psf_parse(d).map(|es| es.into_iter().map(|e| (e.key.clone(), e.as_string())).collect()).unwrap_or_default()
}

pub fn read_sfo(path: &Path) -> Option<BTreeMap<String, String>> {
    fs::read(path).ok().filter(|d| psf_parse(d).is_some()).map(|d| sfo_values(&d))
}

/// RPCS3's `*<file>` keys: the files it marks secure.
fn rpcs3_secure_files(es: &[PsfEntry]) -> Vec<String> {
    es.iter()
        .filter_map(|e| {
            let n = e.key.strip_prefix('*')?;
            (safe_component(n) && e.as_int().unwrap_or(0) != 0).then(|| n.to_string())
        })
        .collect()
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

fn io<T>(r: std::io::Result<T>, p: &Path) -> Result<T, String> {
    r.map_err(|e| format!("{}: {e}", p.display()))
}

fn mtime(p: &Path) -> u64 {
    fs::metadata(p).and_then(|m| m.modified()).ok().and_then(|t| t.duration_since(UNIX_EPOCH).ok()).map(|d| d.as_secs()).unwrap_or(0)
}

/// Copy a file and keep its modification time (the date the list shows).
fn copy_file(src: &Path, dst: &Path) -> Result<(), String> {
    if let Some(d) = dst.parent() {
        io(fs::create_dir_all(d), d)?;
    }
    io(fs::copy(src, dst), src)?;
    if let Ok(t) = fs::metadata(src).and_then(|m| m.modified()) {
        if let Ok(f) = fs::OpenOptions::new().write(true).open(dst) {
            let _ = f.set_modified(t);
        }
    }
    Ok(())
}

/// Copy a folder tree (links are skipped), keeping modification times.
fn copy_tree(src: &Path, dst: &Path) -> Result<(), String> {
    io(fs::create_dir_all(dst), dst)?;
    for e in io(fs::read_dir(src), src)?.flatten() {
        let Ok(ft) = e.file_type() else { continue };
        let (from, to) = (e.path(), dst.join(e.file_name()));
        if ft.is_dir() {
            copy_tree(&from, &to)?;
        } else if ft.is_file() {
            copy_file(&from, &to)?;
        }
    }
    Ok(())
}

/// Every regular file under `dir`, as ('/'-joined relative path, full path).
fn walk(dir: &Path) -> Vec<(String, PathBuf)> {
    fn go(base: &Path, rel: &str, out: &mut Vec<(String, PathBuf)>) {
        let Ok(it) = fs::read_dir(base) else { return };
        for e in it.flatten() {
            let Ok(ft) = e.file_type() else { continue };
            let name = e.file_name().to_string_lossy().into_owned();
            let r = if rel.is_empty() { name.clone() } else { format!("{rel}/{name}") };
            if ft.is_dir() {
                go(&e.path(), &r, out);
            } else if ft.is_file() {
                out.push((r, e.path()));
            }
        }
    }
    let mut out = Vec::new();
    go(dir, "", &mut out);
    out.sort();
    out
}

/// FNV-1a, to tell two saves (or two backups) apart.
fn fnv64(d: &[u8]) -> u64 {
    d.iter().fold(0xcbf2_9ce4_8422_2325u64, |h, &b| (h ^ b as u64).wrapping_mul(0x0100_0000_01b3))
}

/// Swap a slot in whole: build the new one next to it, move the old one
/// aside, rename, and only then delete the old one. A failure on the way
/// leaves the old one where it was.
fn replace_dir(dest: &Path, fill: impl FnOnce(&Path) -> Result<(), String>) -> Result<(), String> {
    let parent = dest.parent().ok_or("no parent folder")?;
    let name = dest.file_name().ok_or("no folder name")?.to_string_lossy().into_owned();
    io(fs::create_dir_all(parent), parent)?;
    let tmp = parent.join(format!(".{name}.incoming"));
    let old = parent.join(format!(".{name}.previous"));
    let _ = fs::remove_dir_all(&tmp);
    io(fs::create_dir_all(&tmp), &tmp)?;
    if let Err(e) = fill(&tmp) {
        let _ = fs::remove_dir_all(&tmp);
        return Err(e);
    }
    let had_old = dest.exists();
    if had_old {
        let _ = fs::remove_dir_all(&old);
        if let Err(e) = fs::rename(dest, &old) {
            let _ = fs::remove_dir_all(&tmp);
            return Err(format!("{}: {e}", dest.display()));
        }
    }
    if let Err(e) = fs::rename(&tmp, dest) {
        if had_old {
            let _ = fs::rename(&old, dest);
        }
        let _ = fs::remove_dir_all(&tmp);
        return Err(format!("{}: {e}", dest.display()));
    }
    if had_old {
        let _ = fs::remove_dir_all(&old);
    }
    Ok(())
}

/// UTC "YYYYMMDD-HHMMSS" for backup folder names.
pub(crate) fn stamp(secs: u64) -> String {
    let (y, m, d) = zipfile::civil_from_days((secs / 86_400) as i64);
    let r = secs % 86_400;
    format!("{y:04}{m:02}{d:02}-{:02}{:02}{:02}", r / 3600, r % 3600 / 60, r % 60)
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

#[derive(Serialize, Clone, Debug, Default)]
#[serde(rename_all = "camelCase")]
pub struct Slot {
    /// `<label>/<folder>`: what the operations take.
    pub key: String,
    /// The folder name (`BCUS98147…`).
    pub name: String,
    /// The save folder it is in (its root's label).
    pub root: String,
    pub title: String,
    pub subtitle: String,
    pub detail: String,
    /// Seconds since the epoch (the newest file).
    pub modified: u64,
    pub bytes: u64,
    pub path: String,
    /// ICON0.PNG as a data URL (when asked for).
    pub icon: Option<String>,
}

fn slot_hash(dir: &Path) -> u64 {
    walk(dir).into_iter().filter(|(r, _)| !r.starts_with('.')).fold(0u64, |h, (r, p)| {
        let d = fs::read(&p).map(|d| fnv64(&d)).unwrap_or(0);
        h.rotate_left(7) ^ fnv64(r.as_bytes()) ^ d
    })
}

fn read_slot(root: &Root, name: &str, icons: bool) -> Slot {
    let dir = root.dir.join(name);
    let files = walk(&dir);
    let v = read_sfo(&dir.join(SFO)).unwrap_or_default();
    let g = |k: &str| v.get(k).cloned().unwrap_or_default();
    let icon = if icons {
        fs::read(dir.join("ICON0.PNG")).ok().filter(|d| d.len() < 2 << 20).map(|d| format!("data:image/png;base64,{}", crate::base64(&d)))
    } else {
        None
    };
    Slot {
        key: format!("{}/{name}", root.label),
        name: name.to_string(),
        root: root.label.clone(),
        title: g("TITLE"),
        subtitle: g("SUB_TITLE"),
        detail: g("DETAIL"),
        modified: files.iter().map(|(_, p)| mtime(p)).max().unwrap_or(0),
        bytes: files.iter().filter_map(|(_, p)| fs::metadata(p).ok()).map(|m| m.len()).sum(),
        path: dir.to_string_lossy().into_owned(),
        icon,
    }
}

/// Every save slot, root by root, folders sorted by name.
pub fn list_slots(loc: &Locations, icons: bool) -> Vec<Slot> {
    let mut out = Vec::new();
    for root in &loc.roots {
        let Ok(it) = fs::read_dir(&root.dir) else { continue };
        let mut names: Vec<String> =
            it.flatten().filter(|e| e.path().is_dir()).map(|e| e.file_name().to_string_lossy().into_owned()).filter(|n| valid_slot(n)).collect();
        names.sort();
        out.extend(names.iter().map(|n| read_slot(root, n, icons)));
    }
    out
}

// ---------------------------------------------------------------------------
// Backups
// ---------------------------------------------------------------------------

#[derive(Serialize, Deserialize, Clone, Debug, Default)]
#[serde(rename_all = "camelCase", default)]
pub struct BackupMeta {
    pub id: String,
    /// Seconds since the epoch.
    pub created: u64,
    /// "auto" (before Play), "manual", "before-restore", "before-import",
    /// "before-delete".
    pub reason: String,
    /// Slot keys (`<label>/<folder>`).
    pub slots: Vec<String>,
    pub titles: BTreeMap<String, String>,
    pub bytes: u64,
    /// The saves' contents, to skip an automatic backup that would repeat
    /// the last one.
    pub digest: String,
}

fn digest(loc: &Locations, keys: &[String]) -> String {
    let mut h = 0u64;
    for k in keys {
        let v = loc.slot_dir(k).map(|d| slot_hash(&d)).unwrap_or(0);
        h = h.rotate_left(9) ^ fnv64(k.as_bytes()) ^ v;
    }
    format!("{h:016x}")
}

/// Back up `keys` (every slot when None) into `<backups>/<stamp>-<reason>/`
/// as `<label>/<folder>/…`, with backup.json. Written to a hidden folder and
/// renamed when complete. None when there is nothing to back up.
pub fn create_backup(loc: &Locations, keys: Option<&[String]>, reason: &str, now: u64) -> Result<Option<BackupMeta>, String> {
    let all: Vec<String> = list_slots(loc, false).into_iter().map(|s| s.key).collect();
    let want: Vec<String> = match keys {
        Some(k) => {
            for key in k {
                split_key(key)?;
            }
            k.iter().filter(|n| all.contains(n)).cloned().collect()
        }
        None => all,
    };
    if want.is_empty() {
        return Ok(None);
    }
    let mut id = format!("{}-{reason}", stamp(now));
    let mut n = 1;
    while loc.backups.join(&id).exists() {
        n += 1;
        id = format!("{}-{reason}-{n}", stamp(now));
    }
    let tmp = loc.backups.join(format!(".{id}.partial"));
    let _ = fs::remove_dir_all(&tmp);
    io(fs::create_dir_all(&tmp), &tmp)?;
    let result = (|| -> Result<BackupMeta, String> {
        let mut meta = BackupMeta { id: id.clone(), created: now, reason: reason.into(), ..Default::default() };
        for k in &want {
            let src = loc.slot_dir(k)?;
            copy_tree(&src, &tmp.join(k))?;
            let v = read_sfo(&src.join(SFO)).unwrap_or_default();
            meta.titles.insert(k.clone(), v.get("SUB_TITLE").filter(|s| !s.is_empty()).or(v.get("TITLE")).cloned().unwrap_or_default());
            meta.slots.push(k.clone());
        }
        meta.bytes = walk(&tmp).iter().filter_map(|(_, p)| fs::metadata(p).ok()).map(|m| m.len()).sum();
        meta.digest = digest(loc, &want);
        io(fs::write(tmp.join(MANIFEST), serde_json::to_string_pretty(&meta).map_err(|e| e.to_string())?), &tmp)?;
        Ok(meta)
    })();
    match result {
        Ok(meta) => {
            io(fs::rename(&tmp, loc.backups.join(&id)), &tmp)?;
            Ok(Some(meta))
        }
        Err(e) => {
            let _ = fs::remove_dir_all(&tmp);
            Err(e)
        }
    }
}

/// Every complete backup, newest first.
pub fn list_backups(loc: &Locations) -> Vec<BackupMeta> {
    let mut v: Vec<BackupMeta> = fs::read_dir(&loc.backups)
        .into_iter()
        .flatten()
        .flatten()
        .filter(|e| !e.file_name().to_string_lossy().starts_with('.'))
        .filter_map(|e| fs::read_to_string(e.path().join(MANIFEST)).ok())
        .filter_map(|t| serde_json::from_str::<BackupMeta>(&t).ok())
        .filter(|m| valid_id(&m.id))
        .collect();
    v.sort_by(|a, b| b.created.cmp(&a.created).then(b.id.cmp(&a.id)));
    v
}

fn backup_dir(loc: &Locations, id: &str) -> Result<PathBuf, String> {
    let d = loc.backups.join(id);
    if valid_id(id) && d.join(MANIFEST).is_file() {
        Ok(d)
    } else {
        Err(format!("no backup {id:?}"))
    }
}

pub fn delete_backup(loc: &Locations, id: &str) -> Result<(), String> {
    let d = backup_dir(loc, id)?;
    io(fs::remove_dir_all(&d), &d)
}

/// Keep the newest `keep` backups made for `reason`; returns how many went.
pub fn prune_backups(loc: &Locations, reason: &str, keep: usize) -> Result<usize, String> {
    let old: Vec<BackupMeta> = list_backups(loc).into_iter().filter(|b| b.reason == reason).skip(keep).collect();
    for b in &old {
        delete_backup(loc, &b.id)?;
    }
    Ok(old.len())
}

/// Before Play: every slot, unless nothing changed since the last automatic
/// backup; the newest `keep` are kept (0 = off).
pub fn backup_before_play(loc: &Locations, keep: u32, now: u64) -> Result<Option<BackupMeta>, String> {
    if keep == 0 {
        return Ok(None);
    }
    let keys: Vec<String> = list_slots(loc, false).into_iter().map(|s| s.key).collect();
    if keys.is_empty() {
        return Ok(None);
    }
    let d = digest(loc, &keys);
    if list_backups(loc).iter().find(|b| b.reason == "auto").is_some_and(|b| b.digest == d) {
        return Ok(None);
    }
    let b = create_backup(loc, None, "auto", now)?;
    prune_backups(loc, "auto", keep as usize)?;
    Ok(b)
}

#[derive(Serialize, Debug, Default)]
#[serde(rename_all = "camelCase")]
pub struct Report {
    /// The saves written.
    pub slots: Vec<String>,
    /// The backup made of what they replaced, if anything was there.
    pub backup: Option<String>,
}

/// Write one slot from `src` (a folder holding its files). `from_rpcs3`: an
/// RPCS3 slot, whose `*<file>` keys become the port's PARAM.PFD.
fn write_slot(loc: &Locations, key: &str, src: &Path, from_rpcs3: bool) -> Result<(), String> {
    let dest = loc.slot_dir(key)?;
    replace_dir(&dest, |tmp| {
        copy_tree(src, tmp)?;
        if from_rpcs3 && !tmp.join(PFD).exists() {
            let secure = fs::read(src.join(SFO)).ok().and_then(|d| psf_parse(&d)).map(|es| rpcs3_secure_files(&es)).unwrap_or_default();
            if !secure.is_empty() {
                io(fs::write(tmp.join(PFD), secure.join("\n") + "\n"), tmp)?;
            }
        }
        Ok(())
    })
}

fn exists(loc: &Locations, key: &str) -> bool {
    loc.slot_dir(key).is_ok_and(|d| d.is_dir())
}

/// Put a backup back where it came from (all of its saves, or one).
pub fn restore_backup(loc: &Locations, id: &str, key: Option<&str>, now: u64) -> Result<Report, String> {
    let dir = backup_dir(loc, id)?;
    let meta: BackupMeta = serde_json::from_str(&io(fs::read_to_string(dir.join(MANIFEST)), &dir)?).map_err(|e| e.to_string())?;
    let slots: Vec<String> = match key {
        Some(s) if meta.slots.iter().any(|m| m == s) => vec![s.to_string()],
        Some(s) => return Err(format!("{s:?} is not in backup {id}")),
        None => meta.slots.clone(),
    };
    for s in &slots {
        split_key(s)?;
    }
    let here: Vec<String> = slots.iter().filter(|s| exists(loc, s)).cloned().collect();
    let backup = create_backup(loc, Some(&here), "before-restore", now)?.map(|b| b.id);
    for s in &slots {
        write_slot(loc, s, &dir.join(s), false)?;
    }
    Ok(Report { slots, backup })
}

/// Delete one save, after a backup that must succeed first.
pub fn delete_slot(loc: &Locations, key: &str, now: u64) -> Result<BackupMeta, String> {
    split_key(key)?;
    let b = create_backup(loc, Some(&[key.to_string()]), "before-delete", now)?.ok_or_else(|| format!("{key}: nothing to delete"))?;
    let p = loc.slot_dir(key)?;
    io(fs::remove_dir_all(&p), &p)?;
    Ok(b)
}

// ---------------------------------------------------------------------------
// .zip
// ---------------------------------------------------------------------------

const ZIP_LIMITS: zipfile::Limits = zipfile::Limits { entries: 4096, file: 64 << 20, total: 256 << 20 };

/// The saves (all, or the keys named) as `<folder>/<file>`, plus a small
/// manifest. Returns how many saves went in.
pub fn export_zip(loc: &Locations, keys: Option<&[String]>, dest: &Path, now: u64) -> Result<usize, String> {
    let all = list_slots(loc, false);
    let want: Vec<&Slot> = all.iter().filter(|s| keys.is_none_or(|w| w.iter().any(|n| *n == s.key))).collect();
    if want.is_empty() {
        return Err("no saves to export".into());
    }
    let mut entries = Vec::new();
    let mut seen = std::collections::BTreeSet::new();
    for s in &want {
        // the same folder in two roots: the first (the main one) goes in
        if !seen.insert(s.name.clone()) {
            continue;
        }
        for (rel, p) in walk(Path::new(&s.path)) {
            entries.push(zipfile::Entry { name: format!("{}/{rel}", s.name), data: io(fs::read(&p), &p)?, mtime: mtime(&p) });
        }
    }
    let manifest = serde_json::json!({
        "game": "Folklore",
        "titles": TITLE_IDS,
        "created": now,
        "saves": seen.iter().collect::<Vec<_>>(),
    });
    entries.push(zipfile::Entry { name: ZIP_MANIFEST.into(), data: manifest.to_string().into_bytes(), mtime: now });
    zipfile::write(dest, &entries)?;
    Ok(seen.len())
}

/// Import a .zip into the main save folder: every `BCUS98147*` /
/// `BCES00050*` folder in it, anywhere in the archive (so a zipped RPCS3
/// `savedata` works). What is there now is backed up first.
pub fn import_zip(loc: &Locations, zip: &Path, now: u64) -> Result<Report, String> {
    let main = loc.roots.first().ok_or("no save folder")?.clone();
    let entries = zipfile::read(zip, &ZIP_LIMITS)?;
    let mut slots: BTreeMap<String, Vec<(String, &zipfile::Entry)>> = BTreeMap::new();
    for e in &entries {
        let parts: Vec<&str> = e.name.split('/').filter(|p| !p.is_empty()).collect();
        if parts.iter().any(|p| *p == "__MACOSX" || !safe_component(p)) {
            continue;
        }
        if let Some(i) = parts.iter().position(|p| valid_slot(p)) {
            if i + 2 == parts.len() {
                slots.entry(parts[i].to_string()).or_default().push((parts[i + 1].to_string(), e));
            }
        }
    }
    if slots.is_empty() {
        return Err("no Folklore saves (BCUS98147… or BCES00050… folders) in this archive".into());
    }
    let keys: Vec<String> = slots.keys().map(|n| format!("{}/{n}", main.label)).collect();
    let here: Vec<String> = keys.iter().filter(|k| exists(loc, k)).cloned().collect();
    let backup = create_backup(loc, Some(&here), "before-import", now)?.map(|b| b.id);
    let stage = loc.backups.join(format!(".import-{}", stamp(now)));
    let _ = fs::remove_dir_all(&stage);
    let result = (|| -> Result<(), String> {
        for (slot, files) in &slots {
            let d = stage.join(slot);
            io(fs::create_dir_all(&d), &d)?;
            for (name, e) in files {
                io(fs::write(d.join(name), &e.data), &d)?;
            }
            let from_rpcs3 = !d.join(PFD).is_file()
                && fs::read(d.join(SFO)).ok().and_then(|s| psf_parse(&s)).is_some_and(|es| es.iter().any(|e| e.key.starts_with('*')));
            write_slot(loc, &format!("{}/{slot}", main.label), &d, from_rpcs3)?;
        }
        Ok(())
    })();
    let _ = fs::remove_dir_all(&stage);
    result?;
    Ok(Report { slots: keys, backup })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn loc(tag: &str) -> (PathBuf, Locations) {
        let root = std::env::temp_dir().join(format!("fl-saves-{tag}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&root);
        let l = Locations {
            roots: vec![
                Root { label: "gamedata".into(), dir: root.join("gamedata/dev_hdd0/home/00000001/savedata") },
                Root { label: "hdd0".into(), dir: root.join("hdd0/home/00000001/savedata") },
            ],
            backups: root.join("backups"),
        };
        (root, l)
    }

    fn put(dir: &Path, files: &[(&str, &[u8])]) {
        fs::create_dir_all(dir).unwrap();
        for (n, d) in files {
            fs::write(dir.join(n), d).unwrap();
        }
    }

    #[test]
    fn names() {
        assert!(valid_slot("BCUS98147-SAVE00"));
        assert!(valid_slot("BCES00050_0"));
        assert!(!valid_slot("BCUS98147"));
        assert!(!valid_slot("BCUS98107-PROFILE"));
        assert!(!valid_slot("BCUS98147/../x"));
        assert!(split_key("gamedata/BCUS98147A").is_ok());
        assert!(split_key("../BCUS98147A").is_err());
        assert!(split_key("BCUS98147A").is_err());
    }

    #[test]
    fn backup_restore_delete() {
        let (root, l) = loc("brd");
        put(&l.roots[0].dir.join("BCUS98147-A"), &[("DATA.BIN", b"one")]);
        put(&l.roots[1].dir.join("BCUS98147-B"), &[("DATA.BIN", b"two")]);
        put(&l.roots[0].dir.join("NPUB12345"), &[("X", b"not ours")]);
        let s = list_slots(&l, false);
        assert_eq!(s.iter().map(|s| s.key.as_str()).collect::<Vec<_>>(), ["gamedata/BCUS98147-A", "hdd0/BCUS98147-B"]);
        let b = backup_before_play(&l, 3, 1000).unwrap().unwrap();
        assert_eq!(b.slots.len(), 2);
        // nothing changed: no second automatic backup
        assert!(backup_before_play(&l, 3, 2000).unwrap().is_none());
        fs::write(l.roots[0].dir.join("BCUS98147-A/DATA.BIN"), b"changed").unwrap();
        let d = delete_slot(&l, "gamedata/BCUS98147-A", 3000).unwrap();
        assert!(!l.roots[0].dir.join("BCUS98147-A").exists());
        restore_backup(&l, &b.id, Some("gamedata/BCUS98147-A"), 4000).unwrap();
        assert_eq!(fs::read(l.roots[0].dir.join("BCUS98147-A/DATA.BIN")).unwrap(), b"one");
        restore_backup(&l, &d.id, None, 5000).unwrap();
        assert_eq!(fs::read(l.roots[0].dir.join("BCUS98147-A/DATA.BIN")).unwrap(), b"changed");
        assert!(delete_slot(&l, "gamedata/../x", 6000).is_err());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn zip_round_trip() {
        let (root, l) = loc("zip");
        put(&l.roots[1].dir.join("BCES00050-S1"), &[("DATA.BIN", b"eu"), ("ICON0.PNG", b"png")]);
        let z = root.join("out.zip");
        assert_eq!(export_zip(&l, None, &z, 100).unwrap(), 1);
        let r = import_zip(&l, &z, 200).unwrap();
        assert_eq!(r.slots, ["gamedata/BCES00050-S1"]);
        assert_eq!(fs::read(l.roots[0].dir.join("BCES00050-S1/DATA.BIN")).unwrap(), b"eu");
        fs::remove_dir_all(root).unwrap();
    }
}
