//! Steam's binary KeyValues ("binary VDF"), as in `userdata/<id>/config/shortcuts.vdf`.
//!
//! A file is a list of `(type byte, key, value)` items ended by `0x08`;
//! keys are NUL-terminated byte strings. The types:
//!
//! | byte | value |
//! |---|---|
//! | `0x00` | a nested list (ended by its own `0x08`) |
//! | `0x01` | a NUL-terminated string |
//! | `0x02` | int32, little-endian |
//! | `0x03` | float32 |
//! | `0x04` | pointer (4 bytes) |
//! | `0x05` | a wide string (UTF-16LE, ended by two NUL bytes) |
//! | `0x06` | colour (4 bytes) |
//! | `0x07` | uint64 |
//! | `0x0A` | int64 |
//!
//! `0x0B` also ends a list (some writers use it). Strings and keys are kept as
//! raw bytes and every value as the bytes it was read from, so whatever this
//! module does not change is written back byte for byte (`write(parse(x)) == x`,
//! tested): entries made by Steam or other tools come out untouched.

/// One value.
#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    List(List),
    /// Without the trailing NUL.
    Str(Vec<u8>),
    Int(i32),
    /// The other fixed-size types, as read: (type byte, bytes).
    Raw(u8, Vec<u8>),
    /// A wide string's UTF-16LE bytes, without the two trailing NULs.
    Wide(Vec<u8>),
}

/// A list of items in file order, plus the byte that ended it (0x08 or 0x0B).
#[derive(Clone, Debug, PartialEq)]
pub struct List {
    pub items: Vec<(Vec<u8>, Value)>,
    pub end: u8,
}

impl Default for List {
    fn default() -> Self {
        List { items: Vec::new(), end: END }
    }
}

const LIST: u8 = 0x00;
const STR: u8 = 0x01;
const INT: u8 = 0x02;
const WIDE: u8 = 0x05;
const END: u8 = 0x08;
const END_ALT: u8 = 0x0B;

fn fixed_size(t: u8) -> Option<usize> {
    match t {
        0x03 | 0x04 | 0x06 => Some(4),
        0x07 | 0x0A => Some(8),
        _ => None,
    }
}

impl List {
    /// The value under `key` (keys compare ASCII case-insensitively, as Steam does).
    pub fn get(&self, key: &str) -> Option<&Value> {
        self.items.iter().find(|(k, _)| k.eq_ignore_ascii_case(key.as_bytes())).map(|(_, v)| v)
    }

    pub fn get_list(&self, key: &str) -> Option<&List> {
        match self.get(key)? {
            Value::List(l) => Some(l),
            _ => None,
        }
    }

    pub fn get_list_mut(&mut self, key: &str) -> Option<&mut List> {
        self.items.iter_mut().find(|(k, _)| k.eq_ignore_ascii_case(key.as_bytes())).and_then(|(_, v)| match v {
            Value::List(l) => Some(l),
            _ => None,
        })
    }

    pub fn get_str(&self, key: &str) -> Option<String> {
        match self.get(key)? {
            Value::Str(s) => Some(String::from_utf8_lossy(s).into_owned()),
            _ => None,
        }
    }

    pub fn get_int(&self, key: &str) -> Option<i32> {
        match self.get(key)? {
            Value::Int(i) => Some(*i),
            _ => None,
        }
    }

    /// Replace the value under `key`, or add it at the end.
    pub fn set(&mut self, key: &str, v: Value) {
        match self.items.iter_mut().find(|(k, _)| k.eq_ignore_ascii_case(key.as_bytes())) {
            Some((_, old)) => *old = v,
            None => self.items.push((key.as_bytes().to_vec(), v)),
        }
    }
}

pub fn str_value(s: &str) -> Value {
    Value::Str(s.as_bytes().to_vec())
}

struct Reader<'a> {
    b: &'a [u8],
    at: usize,
}

impl Reader<'_> {
    fn err<T>(&self, what: &str) -> Result<T, String> {
        Err(format!("binary VDF: {what} at byte {}", self.at))
    }

    fn byte(&mut self) -> Result<u8, String> {
        let Some(&c) = self.b.get(self.at) else { return self.err("unexpected end") };
        self.at += 1;
        Ok(c)
    }

    fn take(&mut self, n: usize) -> Result<&[u8], String> {
        if self.b.len() - self.at < n {
            return self.err("unexpected end");
        }
        let s = &self.b[self.at..self.at + n];
        self.at += n;
        Ok(s)
    }

    fn cstr(&mut self) -> Result<Vec<u8>, String> {
        let Some(n) = self.b[self.at..].iter().position(|&c| c == 0) else { return self.err("unterminated string") };
        let s = self.b[self.at..self.at + n].to_vec();
        self.at += n + 1;
        Ok(s)
    }

    fn wide(&mut self) -> Result<Vec<u8>, String> {
        let start = self.at;
        loop {
            let pair = self.take(2)?;
            if pair == [0, 0] {
                return Ok(self.b[start..self.at - 2].to_vec());
            }
        }
    }

    /// Items up to the list's end byte. `top`: the file's own list, which may
    /// also simply stop at the end of the data.
    fn list(&mut self, depth: u32, top: bool) -> Result<List, String> {
        if depth > 64 {
            return self.err("nested too deep");
        }
        let mut items = Vec::new();
        loop {
            if top && self.at == self.b.len() {
                return Ok(List { items, end: 0xFF });
            }
            let t = self.byte()?;
            if t == END || t == END_ALT {
                return Ok(List { items, end: t });
            }
            let key = self.cstr()?;
            let v = match t {
                LIST => Value::List(self.list(depth + 1, false)?),
                STR => Value::Str(self.cstr()?),
                INT => Value::Int(i32::from_le_bytes(self.take(4)?.try_into().unwrap())),
                WIDE => Value::Wide(self.wide()?),
                t => match fixed_size(t) {
                    Some(n) => Value::Raw(t, self.take(n)?.to_vec()),
                    None => return self.err(&format!("unknown type 0x{t:02x}")),
                },
            };
            items.push((key, v));
        }
    }
}

/// Parse a whole file. Bytes after the top-level end byte are an error (so a
/// file this module does not understand is never rewritten).
pub fn parse(bytes: &[u8]) -> Result<List, String> {
    let mut r = Reader { b: bytes, at: 0 };
    let l = r.list(0, true)?;
    if r.at != bytes.len() {
        return r.err("data after the end");
    }
    Ok(l)
}

fn write_list(out: &mut Vec<u8>, l: &List) {
    for (k, v) in &l.items {
        let t = match v {
            Value::List(_) => LIST,
            Value::Str(_) => STR,
            Value::Int(_) => INT,
            Value::Raw(t, _) => *t,
            Value::Wide(_) => WIDE,
        };
        out.push(t);
        out.extend_from_slice(k);
        out.push(0);
        match v {
            Value::List(l) => write_list(out, l),
            Value::Str(s) => {
                out.extend_from_slice(s);
                out.push(0);
            }
            Value::Int(i) => out.extend_from_slice(&i.to_le_bytes()),
            Value::Raw(_, b) => out.extend_from_slice(b),
            Value::Wide(b) => {
                out.extend_from_slice(b);
                out.extend_from_slice(&[0, 0]);
            }
        }
    }
    // 0xFF marks a top-level list that ended with the data
    if l.end != 0xFF {
        out.push(l.end);
    }
}

pub fn write(l: &List) -> Vec<u8> {
    let mut out = Vec::new();
    write_list(&mut out, l);
    out
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    /// A shortcuts.vdf the way Steam writes it, with two entries (one with
    /// tags and a non-UTF-8 byte in its name) and some types Steam itself does
    /// not use there.
    pub fn sample() -> Vec<u8> {
        let mut b = Vec::new();
        let s = |b: &mut Vec<u8>, k: &str, v: &[u8]| {
            b.push(1);
            b.extend_from_slice(k.as_bytes());
            b.push(0);
            b.extend_from_slice(v);
            b.push(0);
        };
        let i = |b: &mut Vec<u8>, k: &str, v: i32| {
            b.push(2);
            b.extend_from_slice(k.as_bytes());
            b.push(0);
            b.extend_from_slice(&v.to_le_bytes());
        };
        let open = |b: &mut Vec<u8>, k: &str| {
            b.push(0);
            b.extend_from_slice(k.as_bytes());
            b.push(0);
        };
        open(&mut b, "shortcuts");
        open(&mut b, "0");
        i(&mut b, "appid", -1_234_567_890);
        s(&mut b, "AppName", b"Some \xe9mulator");
        s(&mut b, "Exe", b"\"/Applications/Emu.app\"");
        s(&mut b, "StartDir", b"\"/Applications/\"");
        s(&mut b, "icon", b"");
        s(&mut b, "LaunchOptions", b"--fullscreen");
        i(&mut b, "IsHidden", 0);
        i(&mut b, "LastPlayTime", 1_700_000_000);
        open(&mut b, "tags");
        s(&mut b, "0", b"favorite");
        s(&mut b, "1", b"Emulators");
        b.push(8);
        b.push(8);
        open(&mut b, "1");
        i(&mut b, "appid", -42);
        s(&mut b, "AppName", b"Other");
        b.push(3); // float
        b.extend_from_slice(b"f\0");
        b.extend_from_slice(&1.5f32.to_le_bytes());
        b.push(7); // uint64
        b.extend_from_slice(b"u\0");
        b.extend_from_slice(&u64::MAX.to_le_bytes());
        b.push(5); // wide string "hi"
        b.extend_from_slice(b"w\0");
        b.extend_from_slice(&[b'h', 0, b'i', 0, 0, 0]);
        open(&mut b, "tags");
        b.push(8);
        b.push(8);
        b.push(8); // shortcuts
        b.push(8); // file
        b
    }

    #[test]
    fn round_trips_byte_for_byte() {
        let b = sample();
        let l = parse(&b).unwrap();
        assert_eq!(write(&l), b);
        let sc = l.get_list("shortcuts").unwrap();
        assert_eq!(sc.items.len(), 2);
        let e0 = sc.get_list("0").unwrap();
        assert_eq!(e0.get_int("appid"), Some(-1_234_567_890));
        assert_eq!(e0.get_str("exe").as_deref(), Some("\"/Applications/Emu.app\""));
        assert_eq!(e0.get_list("tags").unwrap().items.len(), 2);
        let e1 = sc.get_list("1").unwrap();
        assert_eq!(e1.get("f"), Some(&Value::Raw(3, 1.5f32.to_le_bytes().to_vec())));
        assert_eq!(e1.get("w"), Some(&Value::Wide(vec![b'h', 0, b'i', 0])));
    }

    #[test]
    fn empty_file_steam_writes() {
        let b = b"\0shortcuts\0\x08\x08".to_vec();
        let l = parse(&b).unwrap();
        assert!(l.get_list("shortcuts").unwrap().items.is_empty());
        assert_eq!(write(&l), b);
        // a file that ends without the last end byte is read, and kept so
        let b = b"\0shortcuts\0\x08".to_vec();
        assert_eq!(write(&parse(&b).unwrap()), b);
        assert_eq!(write(&parse(b"").unwrap()), b"");
    }

    #[test]
    fn rejects_what_it_does_not_understand() {
        assert!(parse(b"\0shortcuts\0\x08\x08junk").is_err());
        assert!(parse(b"\x09key\0").is_err());
        assert!(parse(b"\x01key\0no end").is_err());
        assert!(parse(b"\x02key\0\x01\x02").is_err());
        assert!(parse(b"\0a\0\0b\0").is_err());
    }

    #[test]
    fn set_replaces_or_appends() {
        let mut l = List::default();
        l.set("AppName", str_value("a"));
        l.set("appname", str_value("b"));
        assert_eq!(l.items.len(), 1);
        assert_eq!(l.get_str("AppName").as_deref(), Some("b"));
        l.set("appid", Value::Int(7));
        assert_eq!(l.items.len(), 2);
    }
}
