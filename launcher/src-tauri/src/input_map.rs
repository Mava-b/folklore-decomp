//! The controls file the game reads through `PS3_INPUT_MAP` (format in the
//! toolkit's `libs/input/pad_input_map.h`): `[keyboard]`, `[mouse_buttons]`
//! and `[controller]` bind PS3 controls to keys, mouse buttons and SDL
//! controller inputs; `[mouse]` holds the camera settings. The launcher edits
//! the three binding sections and leaves every other section as it found it
//! (the mouse settings are the launcher's own and reach the game as
//! environment variables, which win over the file).

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

/// PS3 control -> inputs, per device. A control the file does not list
/// keeps the game's default; an empty list unbinds it.
#[derive(Serialize, Deserialize, Clone, Debug, Default, PartialEq)]
#[serde(rename_all = "camelCase", default)]
pub struct Bindings {
    pub keyboard: BTreeMap<String, Vec<String>>,
    pub mouse_buttons: BTreeMap<String, Vec<String>>,
    pub controller: BTreeMap<String, Vec<String>>,
}

/// A parsed file: the bindings plus the sections the launcher does not edit,
/// kept verbatim.
#[derive(Debug, Default, PartialEq)]
pub struct InputFile {
    pub bindings: Bindings,
    pub other: String,
    pub has_mouse_section: bool,
}

/// The order controls are written in (the file accepts any).
pub const CONTROLS: &[&str] = &[
    "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3", "start", "select",
    "up", "down", "left", "right", "ls_up", "ls_down", "ls_left", "ls_right", "rs_up", "rs_down",
    "rs_left", "rs_right",
];

#[derive(PartialEq, Clone, Copy)]
enum Sec {
    Preamble,
    Keyboard,
    MouseButtons,
    Controller,
    Other,
}

fn section_of(name: &str) -> Sec {
    match name.trim().to_ascii_lowercase().as_str() {
        "keyboard" | "keys" => Sec::Keyboard,
        "mouse_buttons" | "mouse.buttons" => Sec::MouseButtons,
        "controller" | "gamepad" | "pad" => Sec::Controller,
        _ => Sec::Other,
    }
}

/// Cut a line's comment ('#' or ';' outside quotes).
fn strip_comment(line: &str) -> &str {
    let mut quote: Option<char> = None;
    for (i, c) in line.char_indices() {
        match quote {
            Some(q) if c == q => quote = None,
            Some(_) => {}
            None if c == '"' || c == '\'' => quote = Some(c),
            None if c == '#' || c == ';' => return &line[..i],
            None => {}
        }
    }
    line
}

/// `"a, b"`, `['a', "b"]` or `a,b` -> ["a", "b"]; "" and "none" are no input.
fn split_list(value: &str) -> Vec<String> {
    let flat: String = value.chars().filter(|c| !matches!(c, '"' | '\'' | '[' | ']')).collect();
    flat.split(',')
        .map(str::trim)
        .filter(|n| !n.is_empty() && !n.eq_ignore_ascii_case("none"))
        .map(String::from)
        .collect()
}

pub fn parse(text: &str) -> InputFile {
    let mut out = InputFile::default();
    let mut sec = Sec::Preamble;
    for raw in text.lines() {
        let line = strip_comment(raw).trim();
        if let Some(rest) = line.strip_prefix('[') {
            let name = rest.split(']').next().unwrap_or("");
            sec = section_of(name);
            if sec == Sec::Other {
                if name.trim().eq_ignore_ascii_case("mouse") {
                    out.has_mouse_section = true;
                }
                if !out.other.is_empty() && !out.other.ends_with("\n\n") {
                    out.other.push('\n');
                }
                out.other.push_str(raw.trim_end());
                out.other.push('\n');
            }
            continue;
        }
        match sec {
            Sec::Preamble => {} // the launcher writes its own header
            Sec::Other => {
                out.other.push_str(raw.trim_end());
                out.other.push('\n');
            }
            Sec::Keyboard | Sec::MouseButtons | Sec::Controller => {
                let Some((k, v)) = line.split_once('=') else { continue };
                let key: String = k.chars().filter(|c| !matches!(c, '"' | '\'')).collect();
                let key = key.trim().to_ascii_lowercase();
                if key.is_empty() {
                    continue;
                }
                let map = match sec {
                    Sec::Keyboard => &mut out.bindings.keyboard,
                    Sec::MouseButtons => &mut out.bindings.mouse_buttons,
                    _ => &mut out.bindings.controller,
                };
                map.insert(key, split_list(v));
            }
        }
    }
    while out.other.ends_with("\n\n") {
        out.other.pop();
    }
    out
}

/// Names go into the file between quotes: only plain identifiers.
fn valid_name(n: &str) -> bool {
    !n.is_empty() && n.len() <= 32 && n.chars().all(|c| c.is_ascii_alphanumeric() || c == '_')
}

pub fn validate(b: &Bindings) -> Result<(), String> {
    for map in [&b.keyboard, &b.mouse_buttons, &b.controller] {
        for (k, v) in map {
            if !valid_name(k) {
                return Err(format!("{k:?}: not a control name"));
            }
            if let Some(bad) = v.iter().find(|n| !valid_name(n)) {
                return Err(format!("{bad:?}: not an input name"));
            }
        }
    }
    Ok(())
}

fn write_section(out: &mut String, title: &str, map: &BTreeMap<String, Vec<String>>) {
    if map.is_empty() {
        return;
    }
    out.push_str(&format!("\n[{title}]\n"));
    let known = CONTROLS.iter().filter_map(|c| map.get_key_value(*c));
    let extra = map.iter().filter(|(k, _)| !CONTROLS.contains(&k.as_str()));
    for (k, v) in known.chain(extra) {
        out.push_str(&format!("{k:<8} = \"{}\"\n", v.join(", ")));
    }
}

pub fn render(f: &InputFile) -> String {
    let mut out = String::from(
        "# Folklore: controls, read by the game through PS3_INPUT_MAP.\n\
         # Written by the launcher (Controls); hand edits are fine, but comments in\n\
         # [keyboard], [mouse_buttons] and [controller] are not kept. A control left\n\
         # out keeps its default, \"\" unbinds it. Names: launcher/README.md, \"Controls\".\n",
    );
    write_section(&mut out, "keyboard", &f.bindings.keyboard);
    write_section(&mut out, "mouse_buttons", &f.bindings.mouse_buttons);
    write_section(&mut out, "controller", &f.bindings.controller);
    if !f.other.trim().is_empty() {
        out.push('\n');
        out.push_str(f.other.trim_end());
        out.push('\n');
    }
    out
}

/// What "Play" passes for the file: its path when it exists, and whether it
/// has a `[mouse]` section the launcher's settings must override.
#[derive(Default, Debug, Clone)]
pub struct InputEnv {
    pub path: Option<String>,
    pub has_mouse_section: bool,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_forms_and_keep_other_sections() {
        let f = parse(
            "# header\n\
             [keyboard]\n\
             Cross = \"Space\"   # jump\n\
             start = 'Return, KeypadEnter'\n\
             select = [\"Tab\", \"Backspace\"]\n\
             l3 = \"\"\n\
             r3 = none\n\
             \n\
             [mouse]\n\
             sensitivity = 1.5 # fast\n\
             [mouse_buttons]\n\
             r1 = Left\n\
             [Controller]\n\
             cross = \"b\"\n\
             [future]\n\
             x = 1\n",
        );
        let k = &f.bindings.keyboard;
        assert_eq!(k["cross"], vec!["Space"]);
        assert_eq!(k["start"], vec!["Return", "KeypadEnter"]);
        assert_eq!(k["select"], vec!["Tab", "Backspace"]);
        assert!(k["l3"].is_empty() && k["r3"].is_empty());
        assert_eq!(f.bindings.mouse_buttons["r1"], vec!["Left"]);
        assert_eq!(f.bindings.controller["cross"], vec!["b"]);
        assert!(f.has_mouse_section);
        assert_eq!(f.other, "[mouse]\nsensitivity = 1.5 # fast\n\n[future]\nx = 1\n");
    }

    #[test]
    fn render_round_trips() {
        let mut f = InputFile::default();
        f.bindings.keyboard.insert("start".into(), vec!["Return".into(), "KeypadEnter".into()]);
        f.bindings.keyboard.insert("cross".into(), vec!["F".into()]);
        f.bindings.keyboard.insert("l3".into(), vec![]);
        f.bindings.controller.insert("r1".into(), vec!["righttrigger".into()]);
        f.other = "[mouse]\ninvert_y = true\n".into();
        let text = render(&f);
        // cross before start (the fixed order), the empty list as ""
        let cross = text.find("cross    = \"F\"").unwrap();
        let start = text.find("start    = \"Return, KeypadEnter\"").unwrap();
        assert!(cross < start);
        assert!(text.contains("l3       = \"\""));
        assert!(!text.contains("\n[mouse_buttons]\n")); // nothing to say
        let back = parse(&text);
        assert_eq!(back.bindings, f.bindings);
        assert_eq!(back.other, f.other);
        assert!(back.has_mouse_section);
    }

    #[test]
    fn names_are_checked() {
        let mut b = Bindings::default();
        b.keyboard.insert("cross".into(), vec!["Space".into(), "0x0A".into()]);
        assert!(validate(&b).is_ok());
        b.keyboard.insert("circle".into(), vec!["Q\"\n[x]".into()]);
        assert!(validate(&b).is_err());
        let mut b = Bindings::default();
        b.controller.insert("bad key".into(), vec![]);
        assert!(validate(&b).is_err());
    }
}
