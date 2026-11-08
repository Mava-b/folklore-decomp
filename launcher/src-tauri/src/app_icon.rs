//! The launcher's own Dock icon made from the player's copy (macOS).
//!
//! No game art ships with the app: when the player turns on "Your copy's icon
//! in the Dock", the copy's own `PS3_GAME/ICON0.PNG` (320x176, not square) is
//! drawn centred on a dark rounded square in Apple's icon grid, cached as
//! `<data root>/app-icon.png` and set as the running app's icon with
//! `-[NSApplication setApplicationIconImage:]` at every start. Turning it off
//! hands the Dock back the bundle's placeholder icon.
//!
//! The optional Finder icon (`set_finder_icon`) goes through
//! `-[NSWorkspace setIcon:forFile:options:]`, which writes an `Icon\r` file and
//! a FinderInfo flag into the .app. codesign leaves `Icon\r` at the bundle root
//! out of the seal, so `codesign --verify` still passes; only `--strict` objects
//! (to the FinderInfo attribute), and Gatekeeper has already passed the app by
//! the time the player can press the button. Removing it restores the bundle
//! exactly.

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::UNIX_EPOCH;

/// The canvas the composed icon is drawn on (Apple's icon template).
pub const SIZE: u32 = 1024;
/// Apple's macOS icon grid, on the 1024 canvas: an 824x824 rounded square
/// with a 185.4 corner radius, the rest left for the shadow.
const BODY: f32 = 824.0;
const RADIUS: f32 = 185.4;
/// The art's inset inside the body, its corner radius and the rim's width.
const PAD: f32 = 52.0;
const ART_RADIUS: f32 = 26.0;
const RIM: f32 = 3.0;
/// The redesign's dark panel colours (styles.css: --panel-2 down to --bg)
/// and its line colour for the rim.
const BG_TOP: [f32; 3] = [0x1a as f32, 0x1d as f32, 0x22 as f32];
const BG_BOTTOM: [f32; 3] = [0x0a as f32, 0x0b as f32, 0x0d as f32];
const RIM_RGB: [f32; 3] = [0x34 as f32, 0x3a as f32, 0x43 as f32];
/// Bumped when the drawing changes, so a cached icon is redrawn.
const STYLE: u32 = 1;

/// An 8-bit RGBA image, rows top to bottom, not premultiplied.
#[derive(Clone, Debug, PartialEq)]
pub struct Rgba {
    pub w: u32,
    pub h: u32,
    pub px: Vec<u8>,
}

impl Rgba {
    fn at(&self, x: u32, y: u32) -> [f32; 4] {
        let i = ((y * self.w + x) * 4) as usize;
        let p = &self.px[i..i + 4];
        [p[0] as f32, p[1] as f32, p[2] as f32, p[3] as f32]
    }
}

pub fn decode_png(bytes: &[u8]) -> Result<Rgba, String> {
    let mut dec = png::Decoder::new(std::io::Cursor::new(bytes));
    dec.set_transformations(png::Transformations::EXPAND | png::Transformations::STRIP_16);
    let mut reader = dec.read_info().map_err(|e| e.to_string())?;
    let mut buf = vec![0; reader.output_buffer_size().ok_or("image too large")?];
    let info = reader.next_frame(&mut buf).map_err(|e| e.to_string())?;
    let (w, h) = (info.width, info.height);
    if w == 0 || h == 0 || w > 8192 || h > 8192 {
        return Err(format!("{w}x{h}: not an icon size"));
    }
    let n = (w * h) as usize;
    let mut px = Vec::with_capacity(n * 4);
    let line = info.line_size;
    let ch = match info.color_type {
        png::ColorType::Grayscale => 1,
        png::ColorType::GrayscaleAlpha => 2,
        png::ColorType::Rgb => 3,
        png::ColorType::Rgba => 4,
        png::ColorType::Indexed => return Err("palette image left unexpanded".into()),
    };
    for y in 0..h as usize {
        let row = &buf[y * line..y * line + w as usize * ch];
        for p in row.chunks_exact(ch) {
            px.extend_from_slice(&match ch {
                1 => [p[0], p[0], p[0], 255],
                2 => [p[0], p[0], p[0], p[1]],
                3 => [p[0], p[1], p[2], 255],
                _ => [p[0], p[1], p[2], p[3]],
            });
        }
    }
    Ok(Rgba { w, h, px })
}

pub fn encode_png(img: &Rgba) -> Result<Vec<u8>, String> {
    let mut out = Vec::new();
    let mut enc = png::Encoder::new(&mut out, img.w, img.h);
    enc.set_color(png::ColorType::Rgba);
    enc.set_depth(png::BitDepth::Eight);
    let mut w = enc.write_header().map_err(|e| e.to_string())?;
    w.write_image_data(&img.px).map_err(|e| e.to_string())?;
    w.finish().map_err(|e| e.to_string())?;
    Ok(out)
}

/// Signed distance from (px, py) to a rounded rectangle centred at (cx, cy)
/// with half sizes (hw, hh) and corner radius r: negative inside.
pub(crate) fn rr_dist(px: f32, py: f32, cx: f32, cy: f32, hw: f32, hh: f32, r: f32) -> f32 {
    let qx = (px - cx).abs() - (hw - r);
    let qy = (py - cy).abs() - (hh - r);
    let outside = (qx.max(0.0).powi(2) + qy.max(0.0).powi(2)).sqrt();
    outside + qx.max(qy).min(0.0) - r
}

/// Anti-aliased coverage of a pixel whose centre is `d` from an edge.
pub(crate) fn cover(d: f32) -> f32 {
    (0.5 - d).clamp(0.0, 1.0)
}

/// Where the art goes on an n x n canvas: (x, y, w, h). The art keeps its
/// aspect ratio and fits inside the body less the padding.
pub fn art_rect(w: u32, h: u32, n: u32) -> (f32, f32, f32, f32) {
    let k = n as f32 / SIZE as f32;
    let room = (BODY - 2.0 * PAD) * k;
    let s = room / w.max(h) as f32;
    let (aw, ah) = (w as f32 * s, h as f32 * s);
    ((n as f32 - aw) / 2.0, (n as f32 - ah) / 2.0, aw, ah)
}

/// Bilinear sample of the source at (sx, sy) in its pixel space,
/// premultiplied (so transparent edges do not bleed dark).
pub(crate) fn sample(src: &Rgba, sx: f32, sy: f32) -> [f32; 4] {
    let x = sx.clamp(0.0, (src.w - 1) as f32);
    let y = sy.clamp(0.0, (src.h - 1) as f32);
    let (x0, y0) = (x.floor() as u32, y.floor() as u32);
    let (x1, y1) = ((x0 + 1).min(src.w - 1), (y0 + 1).min(src.h - 1));
    let (fx, fy) = (x - x0 as f32, y - y0 as f32);
    let mut o = [0.0f32; 4];
    for (xx, yy, wgt) in [
        (x0, y0, (1.0 - fx) * (1.0 - fy)),
        (x1, y0, fx * (1.0 - fy)),
        (x0, y1, (1.0 - fx) * fy),
        (x1, y1, fx * fy),
    ] {
        let p = src.at(xx, yy);
        let a = p[3] / 255.0;
        for c in 0..3 {
            o[c] += p[c] * a * wgt;
        }
        o[3] += a * wgt;
    }
    o
}

/// The square icon: the art centred on the dark rounded square, with a rim
/// and a soft shadow, on an n x n transparent canvas.
pub fn compose(src: &Rgba, n: u32) -> Rgba {
    let k = n as f32 / SIZE as f32;
    let c = n as f32 / 2.0;
    let half = BODY * k / 2.0;
    let (r, rim) = (RADIUS * k, RIM * k);
    let (ax, ay, aw, ah) = art_rect(src.w, src.h, n);
    let (acx, acy, ar) = (ax + aw / 2.0, ay + ah / 2.0, ART_RADIUS * k);
    let (shadow_dy, shadow_blur) = (10.0 * k, 28.0 * k);
    let mut px = Vec::with_capacity((n * n * 4) as usize);
    for y in 0..n {
        let py = y as f32 + 0.5;
        let t = ((py - (c - half)) / (2.0 * half)).clamp(0.0, 1.0);
        let bg: [f32; 3] = std::array::from_fn(|i| BG_TOP[i] + (BG_BOTTOM[i] - BG_TOP[i]) * t);
        for x in 0..n {
            let px_ = x as f32 + 0.5;
            let d = rr_dist(px_, py, c, c, half, half, r);
            let body = cover(d);
            // the shadow under the body, a little lower, fading out
            let ds = rr_dist(px_, py - shadow_dy, c, c, half, half, r);
            let shadow = if ds <= 0.0 { 0.38 } else { 0.38 * (1.0 - (ds / shadow_blur).min(1.0)).powi(2) };
            if body <= 0.0 {
                px.extend_from_slice(&[0, 0, 0, (shadow * 255.0).round() as u8]);
                continue;
            }
            // the body: gradient, a thin rim along the edge, then the art
            let rim_a = (body - cover(d + rim)).clamp(0.0, 1.0) / body.max(1e-6);
            let mut rgb: [f32; 3] = std::array::from_fn(|i| bg[i] + (RIM_RGB[i] - bg[i]) * rim_a);
            let art = cover(rr_dist(px_, py, acx, acy, aw / 2.0, ah / 2.0, ar));
            if art > 0.0 {
                let sx = (px_ - ax) / aw * src.w as f32 - 0.5;
                let sy = (py - ay) / ah * src.h as f32 - 0.5;
                let s = sample(src, sx, sy);
                let a = s[3] * art;
                for i in 0..3 {
                    rgb[i] = s[i] * art + rgb[i] * (1.0 - a);
                }
            }
            // the body over its own shadow
            let out_a = body + shadow * (1.0 - body);
            let px_rgb: [u8; 3] = std::array::from_fn(|i| (rgb[i] * body / out_a).round().clamp(0.0, 255.0) as u8);
            px.extend_from_slice(&[px_rgb[0], px_rgb[1], px_rgb[2], (out_a * 255.0).round() as u8]);
        }
    }
    Rgba { w: n, h: n, px }
}

// ---------------------------------------------------------------------------
// The cache
// ---------------------------------------------------------------------------

pub const CACHE_NAME: &str = "app-icon.png";

/// What the cached icon was drawn from: the source's path, size and mtime,
/// and the drawing's version. A different stamp redraws it.
fn stamp(src: &Path) -> Option<String> {
    let m = fs::metadata(src).ok()?;
    let t = m.modified().ok()?.duration_since(UNIX_EPOCH).ok()?.as_secs();
    Some(format!("{}\n{}\n{}\n{STYLE}\n", src.display(), m.len(), t))
}

fn stamp_path(cache: &Path) -> PathBuf {
    cache.with_extension("png.src")
}

/// One drawing at a time (start, a settings change and a status refresh can
/// all ask at once).
static LOCK: Mutex<()> = Mutex::new(());

/// The composed icon at `cache`, drawn again from `src` (the copy's
/// ICON0.PNG) when that changed. Without a source the last cached icon is
/// kept, so it still shows when the copy is not reachable (an unplugged
/// drive). None when there is neither.
pub fn refresh(cache: &Path, src: Option<&Path>) -> Option<PathBuf> {
    let _g = LOCK.lock().unwrap_or_else(|e| e.into_inner());
    if let Some((src, st)) = src.and_then(|s| Some((s, stamp(s)?))) {
        let fresh = cache.is_file() && fs::read_to_string(stamp_path(cache)).ok().as_deref() == Some(st.as_str());
        if !fresh {
            if let Err(e) = draw(cache, src, &st) {
                eprintln!("[launcher] app icon from {}: {e}", src.display());
            }
        }
    }
    cache.is_file().then(|| cache.to_path_buf())
}

fn draw(cache: &Path, src: &Path, st: &str) -> Result<(), String> {
    let img = decode_png(&fs::read(src).map_err(|e| e.to_string())?)?;
    let png = encode_png(&compose(&img, SIZE))?;
    if let Some(d) = cache.parent() {
        fs::create_dir_all(d).map_err(|e| e.to_string())?;
    }
    let tmp = cache.with_extension("png.tmp");
    fs::write(&tmp, png).map_err(|e| e.to_string())?;
    fs::rename(&tmp, cache).map_err(|e| e.to_string())?;
    fs::write(stamp_path(cache), st).map_err(|e| e.to_string())
}

/// What the Dock shows now, so a refresh that changes nothing does not set
/// the same image again: the icon's path and mtime, or "" for the bundle's.
pub fn key(icon: Option<&Path>) -> String {
    let Some(p) = icon else { return String::new() };
    let t = fs::metadata(p).and_then(|m| m.modified()).ok();
    let t = t.and_then(|t| t.duration_since(UNIX_EPOCH).ok()).map_or(0, |d| d.as_nanos());
    format!("{}@{t}", p.display())
}

// ---------------------------------------------------------------------------
// AppKit
// ---------------------------------------------------------------------------

/// The .app the launcher runs from (none in a development build).
pub fn app_bundle() -> Option<PathBuf> {
    let exe = std::env::current_exe().ok()?;
    exe.ancestors()
        .find(|p| p.extension().is_some_and(|e| e == "app") && p.join("Contents").is_dir())
        .map(Path::to_path_buf)
}

/// Whether the bundle carries a custom Finder icon (the `Icon\r` file).
pub fn has_finder_icon(bundle: &Path) -> bool {
    bundle.join("Icon\r").exists()
}

#[cfg(target_os = "macos")]
mod mac {
    use objc2::rc::Retained;
    use objc2::AllocAnyThread;
    use objc2::MainThreadMarker;
    use objc2_app_kit::{NSApplication, NSImage, NSWorkspace, NSWorkspaceIconCreationOptions};
    use objc2_foundation::{NSData, NSString};

    fn image(png: &[u8]) -> Option<Retained<NSImage>> {
        NSImage::initWithData(NSImage::alloc(), &NSData::with_bytes(png))
    }

    /// The running app's Dock icon; None gives back the bundle's. Main
    /// thread only.
    pub fn set_dock_icon(mtm: MainThreadMarker, png: Option<&[u8]>) -> bool {
        let img = match png {
            Some(b) => match image(b) {
                Some(i) => Some(i),
                None => return false,
            },
            None => None,
        };
        let app = NSApplication::sharedApplication(mtm);
        // SAFETY: a valid NSImage or nil, on the main thread.
        unsafe { app.setApplicationIconImage(img.as_deref()) };
        true
    }

    /// The Finder icon of `path`; None removes a custom one.
    pub fn set_file_icon(path: &str, png: Option<&[u8]>) -> bool {
        let img = match png {
            Some(b) => match image(b) {
                Some(i) => Some(i),
                None => return false,
            },
            None => None,
        };
        NSWorkspace::sharedWorkspace().setIcon_forFile_options(
            img.as_deref(),
            &NSString::from_str(path),
            NSWorkspaceIconCreationOptions(0),
        )
    }
}

/// Set the Dock icon to `icon` (a composed PNG), or back to the bundle's.
/// Runs on the main thread; does nothing off macOS.
pub fn apply_dock_icon<R: tauri::Runtime>(app: &tauri::AppHandle<R>, icon: Option<PathBuf>) {
    #[cfg(target_os = "macos")]
    {
        let png = icon.as_ref().and_then(|p| fs::read(p).ok());
        let _ = app.run_on_main_thread(move || {
            if let Some(mtm) = objc2::MainThreadMarker::new() {
                if !mac::set_dock_icon(mtm, png.as_deref()) {
                    eprintln!("[launcher] app icon: AppKit could not read the composed image");
                }
            }
        });
    }
    #[cfg(not(target_os = "macos"))]
    let _ = (app, icon);
}

/// Put `icon` (a composed PNG) on the .app as its Finder icon, or remove the
/// custom one (None).
pub fn set_finder_icon(bundle: &Path, icon: Option<&Path>) -> Result<(), String> {
    #[cfg(target_os = "macos")]
    {
        let png = match icon {
            Some(p) => Some(fs::read(p).map_err(|e| format!("{}: {e}", p.display()))?),
            None => None,
        };
        if mac::set_file_icon(&bundle.to_string_lossy(), png.as_deref()) {
            Ok(())
        } else {
            Err(format!("{}: macOS did not change the icon (is the app in a folder you can write to?)", bundle.display()))
        }
    }
    #[cfg(not(target_os = "macos"))]
    {
        let _ = (bundle, icon);
        Err("the Finder icon is macOS only".into())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A 320x176 stand-in for an ICON0.PNG: opaque red.
    fn icon0() -> Rgba {
        Rgba { w: 320, h: 176, px: [255, 0, 0, 255].repeat(320 * 176) }
    }

    fn pixel(img: &Rgba, x: u32, y: u32) -> [u8; 4] {
        let i = ((y * img.w + x) * 4) as usize;
        img.px[i..i + 4].try_into().unwrap()
    }

    fn reddish(p: [u8; 4]) -> bool {
        p[0] > 200 && p[1] < 40 && p[2] < 40 && p[3] == 255
    }

    #[test]
    fn composes_a_wide_icon_onto_a_square_keeping_its_aspect() {
        let src = icon0();
        // through PNG both ways, as the real file goes
        let src = decode_png(&encode_png(&src).unwrap()).unwrap();
        assert_eq!((src.w, src.h), (320, 176));
        let out = decode_png(&encode_png(&compose(&src, 256)).unwrap()).unwrap();
        assert_eq!((out.w, out.h), (256, 256), "a square comes out");
        assert_eq!(out.px.len(), 256 * 256 * 4);

        // the art's bounding box: the red pixels
        let (mut x0, mut y0, mut x1, mut y1) = (u32::MAX, u32::MAX, 0, 0);
        for y in 0..out.h {
            for x in 0..out.w {
                if reddish(pixel(&out, x, y)) {
                    (x0, y0, x1, y1) = (x0.min(x), y0.min(y), x1.max(x), y1.max(y));
                }
            }
        }
        let (bw, bh) = ((x1 - x0 + 1) as f32, (y1 - y0 + 1) as f32);
        let aspect = 320.0 / 176.0;
        assert!((bw / bh - aspect).abs() < 0.05, "aspect {} != {aspect}", bw / bh);
        // centred
        assert!(((x0 + x1) as i32 - 255).abs() <= 2 && ((y0 + y1) as i32 - 255).abs() <= 2, "{x0},{y0} {x1},{y1}");
        // and as computed by art_rect, inside the body (824/1024 of the side)
        let (ax, _, aw, ah) = art_rect(320, 176, 256);
        assert!((bw - aw).abs() <= 3.0 && (bh - ah).abs() <= 3.0, "{bw}x{bh} vs {aw}x{ah}");
        assert!(ax > (256.0 - 824.0 / 4.0) / 2.0);

        // corners transparent (outside the rounded square), the dark body
        // around the art
        assert_eq!(pixel(&out, 0, 0)[3], 0);
        assert_eq!(pixel(&out, 255, 0)[3], 0);
        let body = pixel(&out, 128, 40);
        assert_eq!(body[3], 255);
        assert!(body[0] < 40 && body[1] < 40 && body[2] < 50, "dark background {body:?}");
        // the body's top-left corner is rounded: the canvas corner of the
        // body square is outside it
        let inset = ((256.0 - 824.0 / 4.0) / 2.0) as u32;
        assert!(pixel(&out, inset + 1, inset + 1)[3] < 255);
    }

    #[test]
    fn full_size_is_square_too() {
        let out = compose(&icon0(), SIZE);
        assert_eq!((out.w, out.h, out.px.len()), (SIZE, SIZE, (SIZE * SIZE * 4) as usize));
        let (_, _, aw, ah) = art_rect(320, 176, SIZE);
        assert!((aw / ah - 320.0 / 176.0).abs() < 1e-3);
        assert!(aw <= BODY);
    }

    #[test]
    fn a_tall_or_grey_image_fits_as_well() {
        let src = Rgba { w: 10, h: 40, px: [128, 128, 128, 255].repeat(400) };
        let (ax, ay, aw, ah) = art_rect(10, 40, 128);
        assert!((ah / aw - 4.0).abs() < 1e-3 && ax > ay);
        let out = compose(&src, 128);
        assert_eq!((out.w, out.h), (128, 128));
    }

    #[test]
    fn cache_redraws_only_when_the_source_changes() {
        let dir = std::env::temp_dir().join(format!("app-icon-test-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let src = dir.join("ICON0.PNG");
        let cache = dir.join("cache").join(CACHE_NAME);
        assert_eq!(refresh(&cache, None), None, "nothing to draw from");
        fs::write(&src, encode_png(&icon0()).unwrap()).unwrap();
        assert_eq!(refresh(&cache, Some(&src)), Some(cache.clone()));
        let k1 = key(Some(&cache));
        assert_eq!(refresh(&cache, Some(&src)), Some(cache.clone()));
        assert_eq!(key(Some(&cache)), k1, "not redrawn when the source is the same");
        // the copy unreachable: the cached icon stays
        assert_eq!(refresh(&cache, Some(&dir.join("gone.png"))), Some(cache.clone()));
        assert_eq!(refresh(&cache, None), Some(cache.clone()));
        let out = decode_png(&fs::read(&cache).unwrap()).unwrap();
        assert_eq!((out.w, out.h), (SIZE, SIZE));
        // a broken source leaves the old icon
        fs::write(&src, b"not a png at all").unwrap();
        assert_eq!(refresh(&cache, Some(&src)), Some(cache.clone()));
        assert_eq!(key(None), "");
        let _ = fs::remove_dir_all(&dir);
    }
}
