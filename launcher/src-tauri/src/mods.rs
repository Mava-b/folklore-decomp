//! Texture packs: folders under `<data>/mods/`, each holding PNGs named by
//! the key of the texture they replace (16 hex digits, then optionally `_`
//! and anything: `3f09a1c2b4d5e6f7.png`, `3f09a1c2b4d5e6f7_logo.png`). The
//! game reads them through PS3_MODS_DIR and PS3_MODS (the toolkit's
//! rsx_draw_engine.c); the Settings keep the enabled packs in priority order,
//! the first winning when two replace the same texture.

use serde::Serialize;
use std::fs;
use std::path::Path;

/// One pack folder, as the Mods card lists it.
#[derive(Serialize, Debug, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct ModPack {
    /// The folder's name (what PS3_MODS lists).
    pub name: String,
    /// Replacement textures in it (PNG files with a key name).
    pub textures: u32,
    /// The first line of an optional `info.txt` in the folder.
    pub description: String,
}

/// A pack file's name: 16 hex digits, no 17th, ending in .png.
pub fn is_texture_name(name: &str) -> bool {
    let b = name.as_bytes();
    b.len() >= 20
        && b[..16].iter().all(u8::is_ascii_hexdigit)
        && !b[16].is_ascii_hexdigit()
        && name.to_ascii_lowercase().ends_with(".png")
}

fn count_textures(dir: &Path, depth: u32) -> u32 {
    let Ok(rd) = fs::read_dir(dir) else { return 0 };
    let mut n = 0;
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().into_owned();
        if name.starts_with('.') {
            continue;
        }
        let p = e.path();
        if p.is_dir() {
            if depth < 6 {
                n += count_textures(&p, depth + 1);
            }
        } else if is_texture_name(&name) {
            n += 1;
        }
    }
    n
}

/// A name the game can take in its comma-separated PS3_MODS list.
pub fn valid_pack_name(n: &str) -> bool {
    let n = n.trim();
    !n.is_empty() && !n.starts_with('.') && !n.contains([',', '/', '\\'])
}

/// The packs in `dir` (created when missing), sorted by name.
pub fn list(dir: &Path) -> Vec<ModPack> {
    let _ = fs::create_dir_all(dir);
    let mut out = Vec::new();
    let Ok(rd) = fs::read_dir(dir) else { return out };
    for e in rd.flatten() {
        let p = e.path();
        let name = e.file_name().to_string_lossy().into_owned();
        if !p.is_dir() || !valid_pack_name(&name) {
            continue;
        }
        let description = fs::read_to_string(p.join("info.txt"))
            .ok()
            .and_then(|t| t.lines().map(str::trim).find(|l| !l.is_empty()).map(|l| l.chars().take(200).collect()))
            .unwrap_or_default();
        out.push(ModPack { textures: count_textures(&p, 0), name, description });
    }
    out.sort_by_key(|p| p.name.to_lowercase());
    out
}

/// PS3_MODS: the enabled packs in order, without duplicates or names the list
/// cannot carry; None when there are none (no packs, the default).
pub fn env_list(enabled: &[String]) -> Option<String> {
    let mut seen: Vec<&str> = Vec::new();
    for n in enabled.iter().map(|n| n.trim()).filter(|n| valid_pack_name(n)) {
        if !seen.contains(&n) {
            seen.push(n);
        }
    }
    (!seen.is_empty()).then(|| seen.join(","))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn texture_names() {
        assert!(is_texture_name("3f09a1c2b4d5e6f7.png"));
        assert!(is_texture_name("3F09A1C2B4D5E6F7_logo.PNG"));
        assert!(!is_texture_name("3f09a1c2b4d5e6f7a.png"), "17 hex digits");
        assert!(!is_texture_name("3f09a1c2b4d5e6.png"));
        assert!(!is_texture_name("3f09a1c2b4d5e6f7.jpg"));
        assert!(!is_texture_name("readme.txt"));
    }

    #[test]
    fn env_list_keeps_order_and_drops_bad_names() {
        let v: Vec<String> = ["HD UI", "a,b", "Warm", "HD UI", "../x", ""].iter().map(|s| s.to_string()).collect();
        assert_eq!(env_list(&v).as_deref(), Some("HD UI,Warm"));
        assert_eq!(env_list(&[]), None);
    }

    #[test]
    fn lists_packs_with_counts() {
        let d = std::env::temp_dir().join(format!("rl-mods-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(d.join("B pack/ui")).unwrap();
        fs::create_dir_all(d.join("a pack")).unwrap();
        fs::write(d.join("B pack/ui/0123456789abcdef_hud.png"), b"x").unwrap();
        fs::write(d.join("B pack/0123456789abcdee.png"), b"x").unwrap();
        fs::write(d.join("B pack/notes.png"), b"x").unwrap();
        fs::write(d.join("B pack/info.txt"), "\nSharper HUD\nmore").unwrap();
        fs::write(d.join("loose.png"), b"x").unwrap();
        let l = list(&d);
        assert_eq!(l.len(), 2);
        assert_eq!(l[0], ModPack { name: "a pack".into(), textures: 0, description: String::new() });
        assert_eq!(l[1], ModPack { name: "B pack".into(), textures: 2, description: "Sharper HUD".into() });
        let _ = fs::remove_dir_all(&d);
    }
}
