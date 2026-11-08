//! A small .zip reader and writer for the saves manager: enough for the
//! archives it writes itself (deflate, UTF-8 names) and for the ones a file
//! manager makes (Finder's "Compress", 7-Zip, Explorer: stored or deflate,
//! with or without data descriptors). No ZIP64 and no encryption: a save set
//! is a few hundred KB.

use flate2::read::DeflateDecoder;
use flate2::write::DeflateEncoder;
use flate2::Compression;
use std::fs;
use std::io::{Read, Write};
use std::path::Path;

#[derive(Debug)]
pub struct Entry {
    /// Path inside the archive, '/'-separated.
    pub name: String,
    pub data: Vec<u8>,
    /// Seconds since the epoch (stored as a DOS date, 2 s resolution, UTC).
    pub mtime: u64,
}

/// What `read` accepts at most.
pub struct Limits {
    pub entries: usize,
    pub file: u64,
    pub total: u64,
}

fn u16le(d: &[u8], o: usize) -> Option<u16> {
    Some(u16::from_le_bytes(d.get(o..o + 2)?.try_into().ok()?))
}
fn u32le(d: &[u8], o: usize) -> Option<u32> {
    Some(u32::from_le_bytes(d.get(o..o + 4)?.try_into().ok()?))
}

/// Days since 1970-01-01 -> (year, month, day), proleptic Gregorian.
pub fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    (yoe + era * 400 + i64::from(m <= 2), m, d)
}

/// (year, month, day) -> days since 1970-01-01.
fn days_from_civil(y: i64, m: u32, d: u32) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = y.div_euclid(400);
    let yoe = y.rem_euclid(400);
    let mp = if m > 2 { m - 3 } else { m + 9 } as i64;
    let doy = (153 * mp + 2) / 5 + d as i64 - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146_097 + doe - 719_468
}

fn dos_time(secs: u64) -> (u16, u16) {
    let days = (secs / 86_400) as i64;
    let rem = secs % 86_400;
    let (y, m, d) = civil_from_days(days);
    if y < 1980 {
        return (0, (1 << 5) | 1); // 1980-01-01 00:00
    }
    let y = y.min(2107);
    let time = ((rem / 3600) << 11 | (rem % 3600 / 60) << 5 | ((rem % 60) / 2)) as u16;
    let date = (((y - 1980) as u32) << 9 | m << 5 | d) as u16;
    (time, date)
}

fn unix_from_dos(time: u16, date: u16) -> u64 {
    let y = 1980 + (date >> 9) as i64;
    let m = ((date >> 5) & 15).clamp(1, 12) as u32;
    let d = (date & 31).max(1) as u32;
    let days = days_from_civil(y, m, d);
    let secs = days * 86_400 + (time >> 11) as i64 * 3600 + ((time >> 5) & 63) as i64 * 60 + (time & 31) as i64 * 2;
    secs.max(0) as u64
}

/// Write `entries` to `path` (through a temporary file, so a failed write
/// leaves no half archive behind).
pub fn write(path: &Path, entries: &[Entry]) -> Result<(), String> {
    let mut out: Vec<u8> = Vec::new();
    let mut central: Vec<u8> = Vec::new();
    for e in entries {
        let name = e.name.as_bytes();
        if name.len() > u16::MAX as usize {
            return Err(format!("{}: name too long", e.name));
        }
        let crc = crc32fast::hash(&e.data);
        let mut enc = DeflateEncoder::new(Vec::new(), Compression::default());
        enc.write_all(&e.data).map_err(|e| e.to_string())?;
        let deflated = enc.finish().map_err(|e| e.to_string())?;
        let (method, body): (u16, &[u8]) = if deflated.len() < e.data.len() { (8, &deflated) } else { (0, &e.data) };
        if out.len() > (u32::MAX as usize).saturating_sub(body.len() + 4096) || e.data.len() > u32::MAX as usize {
            return Err("archive too large".into());
        }
        let (t, d) = dos_time(e.mtime);
        let offset = out.len() as u32;
        // local file header
        out.extend_from_slice(&0x0403_4b50u32.to_le_bytes());
        out.extend_from_slice(&20u16.to_le_bytes()); // version needed
        out.extend_from_slice(&0x0800u16.to_le_bytes()); // UTF-8 names
        out.extend_from_slice(&method.to_le_bytes());
        out.extend_from_slice(&t.to_le_bytes());
        out.extend_from_slice(&d.to_le_bytes());
        out.extend_from_slice(&crc.to_le_bytes());
        out.extend_from_slice(&(body.len() as u32).to_le_bytes());
        out.extend_from_slice(&(e.data.len() as u32).to_le_bytes());
        out.extend_from_slice(&(name.len() as u16).to_le_bytes());
        out.extend_from_slice(&0u16.to_le_bytes());
        out.extend_from_slice(name);
        out.extend_from_slice(body);
        // central directory record
        central.extend_from_slice(&0x0201_4b50u32.to_le_bytes());
        central.extend_from_slice(&0x0314u16.to_le_bytes()); // made by: unix, 2.0
        central.extend_from_slice(&20u16.to_le_bytes());
        central.extend_from_slice(&0x0800u16.to_le_bytes());
        central.extend_from_slice(&method.to_le_bytes());
        central.extend_from_slice(&t.to_le_bytes());
        central.extend_from_slice(&d.to_le_bytes());
        central.extend_from_slice(&crc.to_le_bytes());
        central.extend_from_slice(&(body.len() as u32).to_le_bytes());
        central.extend_from_slice(&(e.data.len() as u32).to_le_bytes());
        central.extend_from_slice(&(name.len() as u16).to_le_bytes());
        central.extend_from_slice(&[0u8; 8]); // extra, comment, disk, internal attributes
        central.extend_from_slice(&((0o100644u32) << 16).to_le_bytes()); // -rw-r--r--
        central.extend_from_slice(&offset.to_le_bytes());
        central.extend_from_slice(name);
    }
    if entries.len() > u16::MAX as usize {
        return Err("too many files".into());
    }
    let cd_offset = out.len() as u32;
    let cd_size = central.len() as u32;
    out.extend_from_slice(&central);
    out.extend_from_slice(&0x0605_4b50u32.to_le_bytes());
    out.extend_from_slice(&[0u8; 4]); // this disk, central directory disk
    out.extend_from_slice(&(entries.len() as u16).to_le_bytes());
    out.extend_from_slice(&(entries.len() as u16).to_le_bytes());
    out.extend_from_slice(&cd_size.to_le_bytes());
    out.extend_from_slice(&cd_offset.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());

    let tmp = path.with_extension("zip.partial");
    fs::write(&tmp, &out).map_err(|e| format!("{}: {e}", tmp.display()))?;
    fs::rename(&tmp, path).map_err(|e| {
        let _ = fs::remove_file(&tmp);
        format!("{}: {e}", path.display())
    })
}

/// Every file in the archive (directories are left out), checked against
/// `lim` and against each entry's CRC.
pub fn read(path: &Path, lim: &Limits) -> Result<Vec<Entry>, String> {
    let meta = fs::metadata(path).map_err(|e| format!("{}: {e}", path.display()))?;
    if meta.len() > lim.total.saturating_add(1 << 20) {
        return Err(format!("{}: too large for a save archive", path.display()));
    }
    let d = fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let bad = || format!("{}: not a zip file (or a damaged one)", path.display());
    // end of central directory: the last 22 bytes, or up to 64 KB earlier with a comment
    let lo = d.len().saturating_sub(22 + 0xffff);
    let eocd = (lo..=d.len().saturating_sub(22))
        .rev()
        .find(|&i| u32le(&d, i) == Some(0x0605_4b50))
        .ok_or_else(bad)?;
    let count = u16le(&d, eocd + 10).ok_or_else(bad)? as usize;
    let cd_offset = u32le(&d, eocd + 16).ok_or_else(bad)? as usize;
    if count == 0xffff || cd_offset == 0xffff_ffff {
        return Err(format!("{}: ZIP64 archives are not supported", path.display()));
    }
    if count > lim.entries {
        return Err(format!("{}: too many files ({count})", path.display()));
    }
    let mut out = Vec::new();
    let mut total: u64 = 0;
    let mut p = cd_offset;
    for _ in 0..count {
        if u32le(&d, p) != Some(0x0201_4b50) {
            return Err(bad());
        }
        let flags = u16le(&d, p + 8).ok_or_else(bad)?;
        let method = u16le(&d, p + 10).ok_or_else(bad)?;
        let (t, dt) = (u16le(&d, p + 12).ok_or_else(bad)?, u16le(&d, p + 14).ok_or_else(bad)?);
        let crc = u32le(&d, p + 16).ok_or_else(bad)?;
        let csize = u32le(&d, p + 20).ok_or_else(bad)? as u64;
        let usize_ = u32le(&d, p + 24).ok_or_else(bad)? as u64;
        let nlen = u16le(&d, p + 28).ok_or_else(bad)? as usize;
        let xlen = u16le(&d, p + 30).ok_or_else(bad)? as usize;
        let clen = u16le(&d, p + 32).ok_or_else(bad)? as usize;
        let local = u32le(&d, p + 42).ok_or_else(bad)? as usize;
        let name_bytes = d.get(p + 46..p + 46 + nlen).ok_or_else(bad)?;
        let name = String::from_utf8_lossy(name_bytes).replace('\\', "/");
        p += 46 + nlen + xlen + clen;
        if name.ends_with('/') {
            continue;
        }
        if flags & 1 != 0 {
            return Err(format!("{name}: encrypted archives are not supported"));
        }
        if csize == 0xffff_ffff || usize_ == 0xffff_ffff {
            return Err(format!("{}: ZIP64 archives are not supported", path.display()));
        }
        if usize_ > lim.file {
            return Err(format!("{name}: too large for a save file"));
        }
        total += usize_;
        if total > lim.total {
            return Err(format!("{}: too large for a save archive", path.display()));
        }
        if u32le(&d, local) != Some(0x0403_4b50) {
            return Err(bad());
        }
        let start = local + 30 + u16le(&d, local + 26).ok_or_else(bad)? as usize + u16le(&d, local + 28).ok_or_else(bad)? as usize;
        let body = d.get(start..start + csize as usize).ok_or_else(bad)?;
        let data = match method {
            0 => body.to_vec(),
            8 => {
                let mut v = Vec::with_capacity(usize_ as usize);
                DeflateDecoder::new(body)
                    .take(lim.file + 1)
                    .read_to_end(&mut v)
                    .map_err(|e| format!("{name}: {e}"))?;
                v
            }
            m => return Err(format!("{name}: compression method {m} is not supported")),
        };
        if data.len() as u64 != usize_ || crc32fast::hash(&data) != crc {
            return Err(format!("{name}: damaged (size or CRC mismatch)"));
        }
        out.push(Entry { name, data, mtime: unix_from_dos(t, dt) });
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dates_round_trip() {
        assert_eq!(civil_from_days(0), (1970, 1, 1));
        assert_eq!(civil_from_days(20_743), (2026, 10, 17));
        assert_eq!(days_from_civil(2026, 10, 17), 20_743);
        let t = 1_792_245_722; // 2026-10-17 ... an even second
        let (tm, dt) = dos_time(t);
        assert_eq!(unix_from_dos(tm, dt), t);
    }

    #[test]
    fn write_then_read() {
        let dir = std::env::temp_dir().join(format!("rl-zip-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let p = dir.join("a.zip");
        let big = vec![7u8; 40_000]; // compresses
        let entries = vec![
            Entry { name: "S/STATE".into(), data: big.clone(), mtime: 1_700_000_000 },
            Entry { name: "S/ICON0.PNG".into(), data: vec![1, 2, 3], mtime: 1_700_000_002 }, // stored
            Entry { name: "Ação/x".into(), data: Vec::new(), mtime: 0 },
        ];
        write(&p, &entries).unwrap();
        let lim = Limits { entries: 10, file: 1 << 20, total: 1 << 22 };
        let back = read(&p, &lim).unwrap();
        assert_eq!(back.len(), 3);
        assert_eq!(back[0].name, "S/STATE");
        assert_eq!(back[0].data, big);
        assert_eq!(back[0].mtime, 1_700_000_000);
        assert_eq!(back[1].data, vec![1, 2, 3]);
        assert_eq!(back[2].name, "Ação/x");
        assert!(fs::metadata(&p).unwrap().len() < 2_000);
        // limits and damage are refused
        assert!(read(&p, &Limits { entries: 2, file: 1 << 20, total: 1 << 22 }).is_err());
        assert!(read(&p, &Limits { entries: 10, file: 1000, total: 1 << 22 }).is_err());
        let mut raw = fs::read(&p).unwrap();
        let i = raw.windows(9).position(|w| w == b"S/ICON0.P").unwrap() + 11; // the stored bytes
        raw[i] ^= 0xff;
        fs::write(&p, &raw).unwrap();
        assert!(read(&p, &lim).unwrap_err().contains("damaged"));
        fs::write(&p, b"not a zip").unwrap();
        assert!(read(&p, &lim).is_err());
        fs::remove_dir_all(&dir).unwrap();
    }
}
