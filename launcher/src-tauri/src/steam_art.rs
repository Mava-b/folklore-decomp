//! Steam library art for the "Add to Steam" shortcut, drawn at runtime from
//! the player's own copy (`PS3_GAME/PIC1.PNG`, the 1920x1080 background, and
//! `ICON0.PNG`, the 320x176 title card). Nothing from the game ships with the
//! app; this only resizes and composes the player's files, in the launcher's
//! dark look with its red accent:
//!
//! - portrait capsule `<appid>p.png`, 600x900, and landscape capsule
//!   `<appid>.png`, 920x430: PIC1 cropped to fill, blurred and darkened, a
//!   accent glow, and ICON0 centred on it as a card (rounded corners, a thin
//!   accent rim, a soft shadow), with a short accent line under it;
//! - hero `<appid>_hero.png`, 1920x620: PIC1 cropped to fill (its upper
//!   part), sharp, a little darker towards the bottom (Steam writes the
//!   title over it);
//! - icon `<appid>_icon.png`, 256x256: the same square as the launcher's
//!   Dock icon (app_icon::compose).
//!
//! Without PIC1 the backgrounds are made from ICON0 itself.

use crate::app_icon::{compose, cover, rr_dist, sample, Rgba};

pub const PORTRAIT: (u32, u32) = (600, 900);
pub const LANDSCAPE: (u32, u32) = (920, 430);
pub const HERO: (u32, u32) = (1920, 620);
pub const ICON: u32 = 256;

/// styles.css: --bg, --accent, --accent-deep.
const BG: [f32; 3] = [9.0, 12.0, 13.0];
const ACCENT: [f32; 3] = [108.0, 198.0, 168.0];
const ACCENT_DEEP: [f32; 3] = [46.0, 122.0, 99.0];
/// The accent glow's strength at its centre.
const GLOW: f32 = 0.36;
/// Where the hero's crop sits down PIC1: a third of the way, which keeps the
/// audio trademarks printed in its lower middle out of the banner.
const HERO_TOP: f32 = 0.33;

/// A float RGB canvas (0..255), opaque.
struct Canvas {
    w: u32,
    h: u32,
    px: Vec<[f32; 3]>,
}

impl Canvas {
    fn to_rgba(&self) -> Rgba {
        let mut px = Vec::with_capacity(self.px.len() * 4);
        for p in &self.px {
            px.extend_from_slice(&[q(p[0]), q(p[1]), q(p[2]), 255]);
        }
        Rgba { w: self.w, h: self.h, px }
    }
}

fn q(v: f32) -> u8 {
    v.round().clamp(0.0, 255.0) as u8
}

fn smoothstep(a: f32, b: f32, x: f32) -> f32 {
    let t = ((x - a) / (b - a)).clamp(0.0, 1.0);
    t * t * (3.0 - 2.0 * t)
}

/// The source cropped to the target's aspect ratio and resampled to w x h.
/// The crop is centred across; `top` places it down the image (0 = the top,
/// 0.5 = centred). Shrinking averages a grid of bilinear taps over each
/// output pixel's footprint, so a 1920-wide image does not alias at 75 pixels.
fn fill(src: &Rgba, w: u32, h: u32, top: f32) -> Canvas {
    let s = (w as f32 / src.w as f32).max(h as f32 / src.h as f32);
    let (cw, ch) = (w as f32 / s, h as f32 / s); // the crop, in source pixels
    let (ox, oy) = ((src.w as f32 - cw) / 2.0, (src.h as f32 - ch) * top);
    let step = 1.0 / s; // source pixels per output pixel
    let taps = step.ceil().clamp(1.0, 16.0) as u32;
    let mut px = Vec::with_capacity((w * h) as usize);
    for y in 0..h {
        for x in 0..w {
            let mut acc = [0.0f32; 4];
            for ty in 0..taps {
                for tx in 0..taps {
                    let sx = ox + (x as f32 + (tx as f32 + 0.5) / taps as f32) * step - 0.5;
                    let sy = oy + (y as f32 + (ty as f32 + 0.5) / taps as f32) * step - 0.5;
                    let p = sample(src, sx, sy);
                    for i in 0..4 {
                        acc[i] += p[i];
                    }
                }
            }
            let n = (taps * taps) as f32;
            // premultiplied over the dark background (an opaque PIC1 is unchanged)
            let a = acc[3] / n;
            px.push(std::array::from_fn(|i| acc[i] / n + BG[i] * (1.0 - a)));
        }
    }
    Canvas { w, h, px }
}

/// Three box blurs of radius r in each direction (close to a Gaussian).
fn blur(c: &mut Canvas, r: i32) {
    let (w, h) = (c.w as i32, c.h as i32);
    let mut tmp = c.px.clone();
    for _ in 0..3 {
        box_pass(&c.px, &mut tmp, w, h, r, true);
        box_pass(&tmp, &mut c.px, w, h, r, false);
    }
}

fn box_pass(src: &[[f32; 3]], dst: &mut [[f32; 3]], w: i32, h: i32, r: i32, horizontal: bool) {
    let (len, lines) = if horizontal { (w, h) } else { (h, w) };
    let at = |line: i32, i: i32| -> usize {
        let i = i.clamp(0, len - 1);
        if horizontal {
            (line * w + i) as usize
        } else {
            (i * w + line) as usize
        }
    };
    let n = (2 * r + 1) as f32;
    for line in 0..lines {
        let mut sum = [0.0f32; 3];
        for i in -r..=r {
            let p = src[at(line, i)];
            for k in 0..3 {
                sum[k] += p[k];
            }
        }
        for i in 0..len {
            dst[at(line, i)] = std::array::from_fn(|k| sum[k] / n);
            let (add, sub) = (src[at(line, i + r + 1)], src[at(line, i - r)]);
            for k in 0..3 {
                sum[k] += add[k] - sub[k];
            }
        }
    }
}

/// A blurred, darkened backdrop of w x h: the source filled at an eighth of
/// the size, blurred there and scaled up (smooth and cheap).
fn backdrop(src: &Rgba, w: u32, h: u32) -> Canvas {
    let (sw, sh) = ((w / 8).max(4), (h / 8).max(4));
    let mut small = fill(src, sw, sh, 0.5);
    blur(&mut small, 2);
    let small = small.to_rgba();
    let mut px = Vec::with_capacity((w * h) as usize);
    for y in 0..h {
        for x in 0..w {
            let sx = (x as f32 + 0.5) * sw as f32 / w as f32 - 0.5;
            let sy = (y as f32 + 0.5) * sh as f32 / h as f32 - 0.5;
            let p = sample(&small, sx, sy);
            px.push([p[0], p[1], p[2]]);
        }
    }
    Canvas { w, h, px }
}

/// Darken towards the launcher's background, more at the bottom, and light
/// a accent glow around (gx, gy).
fn shade(c: &mut Canvas, gx: f32, gy: f32, glow_r: f32) {
    let h = c.h as f32;
    for y in 0..c.h {
        let fy = (y as f32 + 0.5) / h;
        let dark = 0.42 + 0.38 * smoothstep(0.5, 1.0, fy) + 0.1 * (1.0 - smoothstep(0.0, 0.25, fy));
        for x in 0..c.w {
            let p = &mut c.px[(y * c.w + x) as usize];
            let (dx, dy) = ((x as f32 + 0.5 - gx) / glow_r, (y as f32 + 0.5 - gy) / glow_r);
            let g = (1.0 - (dx * dx + dy * dy).sqrt()).clamp(0.0, 1.0).powi(2) * GLOW;
            for i in 0..3 {
                let v = p[i] + (BG[i] - p[i]) * dark.min(0.94);
                // screen blend of the glow, deep accent at its edge
                let glow = ACCENT_DEEP[i] + (ACCENT[i] - ACCENT_DEEP[i]) * g / GLOW;
                p[i] = 255.0 - (255.0 - v) * (1.0 - g * glow / 255.0);
            }
        }
    }
}

/// The title card centred at (cx, cy), `cw` wide (aspect kept): rounded
/// corners, a soft shadow, a thin accent rim; then a short accent line under it.
fn card(c: &mut Canvas, art: &Rgba, cx: f32, cy: f32, cw: f32) {
    let ch = cw * art.h as f32 / art.w as f32;
    let (hw, hh) = (cw / 2.0, ch / 2.0);
    let k = cw / 500.0;
    let (radius, rim, blur_r, drop) = (12.0 * k, 2.0 * k, 34.0 * k, 12.0 * k);
    let line_y = cy + hh + 34.0 * k;
    let (line_hw, line_hh) = (60.0 * k, 1.6 * k);
    for y in 0..c.h {
        let py = y as f32 + 0.5;
        for x in 0..c.w {
            let px_ = x as f32 + 0.5;
            let p = &mut c.px[(y * c.w + x) as usize];
            // shadow
            let ds = rr_dist(px_, py - drop, cx, cy, hw, hh, radius);
            let sh = if ds <= 0.0 { 0.6 } else { 0.6 * (1.0 - (ds / blur_r).min(1.0)).powi(2) };
            for i in 0..3 {
                p[i] *= 1.0 - sh;
            }
            // the line, with a little glow
            let dl = rr_dist(px_, py, cx, line_y, line_hw, line_hh, line_hh);
            let line = cover(dl) + 0.35 * (1.0 - (dl.max(0.0) / (8.0 * k)).min(1.0)).powi(2);
            let line = line.min(1.0);
            for i in 0..3 {
                p[i] += (ACCENT[i] - p[i]) * line;
            }
            // the card
            let d = rr_dist(px_, py, cx, cy, hw, hh, radius);
            let body = cover(d);
            if body <= 0.0 {
                continue;
            }
            let sx = (px_ - (cx - hw)) / cw * art.w as f32 - 0.5;
            let sy = (py - (cy - hh)) / ch * art.h as f32 - 0.5;
            let s = sample(art, sx, sy);
            let mut rgb: [f32; 3] = std::array::from_fn(|i| s[i] + BG[i] * (1.0 - s[3]));
            let rim_a = ((body - cover(d + rim)) / body.max(1e-6)).clamp(0.0, 1.0) * 0.7;
            for i in 0..3 {
                rgb[i] += (ACCENT[i] - rgb[i]) * rim_a;
                p[i] += (rgb[i] - p[i]) * body;
            }
        }
    }
}

/// The four images, as RGBA. `icon0`: the copy's ICON0.PNG; `pic1`: its
/// PIC1.PNG when there is one (else the backgrounds come from ICON0).
pub struct Images {
    pub portrait: Rgba,
    pub landscape: Rgba,
    pub hero: Rgba,
    pub icon: Rgba,
}

fn capsule(icon0: &Rgba, bg: &Rgba, (w, h): (u32, u32), card_w: f32, cy: f32) -> Rgba {
    let mut c = backdrop(bg, w, h);
    let (cx, cy) = (w as f32 / 2.0, cy);
    shade(&mut c, cx, cy, w.max(h) as f32 * 0.55);
    card(&mut c, icon0, cx, cy, card_w);
    c.to_rgba()
}

pub fn draw(icon0: &Rgba, pic1: Option<&Rgba>) -> Images {
    let bg = pic1.unwrap_or(icon0);
    let (pw, ph) = PORTRAIT;
    let (lw, lh) = LANDSCAPE;
    let portrait = capsule(icon0, bg, PORTRAIT, pw as f32 * 0.84, ph as f32 * 0.46);
    // the card's height at most ~62% of the landscape capsule
    let lcard = (lw as f32 * 0.6).min(lh as f32 * 0.62 * icon0.w as f32 / icon0.h as f32);
    let landscape = capsule(icon0, bg, LANDSCAPE, lcard, lh as f32 * 0.47);
    let hero = match pic1 {
        Some(p) => {
            let (w, h) = HERO;
            let mut c = fill(p, w, h, HERO_TOP);
            for y in 0..h {
                let k = 0.08 + 0.32 * smoothstep(0.45, 1.0, (y as f32 + 0.5) / h as f32);
                for x in 0..w {
                    let px = &mut c.px[(y * w + x) as usize];
                    for i in 0..3 {
                        px[i] += (BG[i] - px[i]) * k;
                    }
                }
            }
            c.to_rgba()
        }
        None => capsule(icon0, icon0, HERO, HERO.1 as f32 * 0.6 * icon0.w as f32 / icon0.h as f32, HERO.1 as f32 * 0.47),
    };
    Images { portrait, landscape, hero, icon: compose(icon0, ICON) }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A stand-in for a copy's art: a gradient with a bright square (no game art in the repo).
    fn fake(w: u32, h: u32, alpha: bool) -> Rgba {
        let mut px = Vec::with_capacity((w * h * 4) as usize);
        for y in 0..h {
            for x in 0..w {
                let inside = x > w / 3 && x < 2 * w / 3 && y > h / 3 && y < 2 * h / 3;
                let a = if alpha && x < 4 { 0 } else { 255 };
                px.extend_from_slice(&if inside {
                    [250, 240, 230, a]
                } else {
                    [(x * 255 / w) as u8, (y * 255 / h) as u8, 90, a]
                });
            }
        }
        Rgba { w, h, px }
    }

    #[test]
    fn sizes_and_opacity() {
        let icon0 = fake(320, 176, true);
        let pic1 = fake(1920, 1080, false);
        for imgs in [draw(&icon0, Some(&pic1)), draw(&icon0, None)] {
            assert_eq!((imgs.portrait.w, imgs.portrait.h), PORTRAIT);
            assert_eq!((imgs.landscape.w, imgs.landscape.h), LANDSCAPE);
            assert_eq!((imgs.hero.w, imgs.hero.h), HERO);
            assert_eq!((imgs.icon.w, imgs.icon.h), (ICON, ICON));
            for img in [&imgs.portrait, &imgs.landscape, &imgs.hero] {
                assert_eq!(img.px.len(), (img.w * img.h * 4) as usize);
                assert!(img.px.chunks_exact(4).all(|p| p[3] == 255), "capsules are opaque");
            }
        }
    }

    #[test]
    fn card_sits_in_the_middle() {
        let icon0 = fake(320, 176, false);
        let pic1 = Rgba { w: 1920, h: 1080, px: [0, 0, 0, 255].repeat(1920 * 1080) };
        let imgs = draw(&icon0, Some(&pic1));
        let at = |img: &Rgba, x: u32, y: u32| {
            let i = ((y * img.w + x) * 4) as usize;
            [img.px[i], img.px[i + 1], img.px[i + 2]]
        };
        // the card's centre is the fake icon's bright square; the corners are dark
        let p = &imgs.portrait;
        assert!(at(p, p.w / 2, (p.h as f32 * 0.46) as u32)[0] > 200);
        assert!(at(p, 2, 2).iter().all(|&c| c < 40));
        let l = &imgs.landscape;
        assert!(at(l, l.w / 2, (l.h as f32 * 0.47) as u32)[0] > 200);
        assert!(at(l, l.w - 3, l.h - 3).iter().all(|&c| c < 40));
    }

    #[test]
    fn blur_keeps_a_flat_image_flat() {
        let mut c = Canvas { w: 9, h: 5, px: vec![[100.0, 50.0, 25.0]; 45] };
        blur(&mut c, 2);
        assert!(c.px.iter().all(|p| (p[0] - 100.0).abs() < 1e-3 && (p[2] - 25.0).abs() < 1e-3));
    }

    /// The four files from a real copy, for a look by eye:
    /// FOLKLORE_ART_ICON0=... FOLKLORE_ART_PIC1=... FOLKLORE_ART_OUT=<dir> cargo test art_from_files -- --ignored
    #[test]
    #[ignore]
    fn art_from_files() {
        let read = |k: &str| std::env::var(k).ok().map(|p| crate::app_icon::decode_png(&std::fs::read(p).unwrap()).unwrap());
        let icon0 = read("FOLKLORE_ART_ICON0").expect("FOLKLORE_ART_ICON0");
        let pic1 = read("FOLKLORE_ART_PIC1");
        let out = std::path::PathBuf::from(std::env::var("FOLKLORE_ART_OUT").expect("FOLKLORE_ART_OUT"));
        let imgs = draw(&icon0, pic1.as_ref());
        for (n, img) in [("p.png", &imgs.portrait), (".png", &imgs.landscape), ("_hero.png", &imgs.hero), ("_icon.png", &imgs.icon)] {
            std::fs::write(out.join(format!("test{n}")), crate::app_icon::encode_png(img).unwrap()).unwrap();
        }
    }
}
