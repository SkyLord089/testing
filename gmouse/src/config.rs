//! Конфигурация и минимальный JSON-парсер/сериализатор.
//!
//! Крейтов нет (нет доступа к crates.io при сборке), поэтому JSON свой —
//! ровно под формат конфига и профилей. Он же разбирает отчёты `diag`.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

// ---------------------------------------------------------------- JSON
#[derive(Debug, Clone, PartialEq)]
pub enum Json {
    Null,
    Bool(bool),
    Num(f64),
    Str(String),
    Arr(Vec<Json>),
    Obj(BTreeMap<String, Json>),
}

impl Json {
    pub fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Obj(map) => map.get(key),
            _ => None,
        }
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            Json::Str(s) => Some(s),
            _ => None,
        }
    }

    pub fn as_num(&self) -> Option<f64> {
        match self {
            Json::Num(n) => Some(*n),
            _ => None,
        }
    }

    pub fn as_int(&self) -> Option<i64> {
        self.as_num().map(|n| n as i64)
    }

    #[allow(dead_code)] // нужен при разборе отчётов и профилей
    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Json::Bool(b) => Some(*b),
            _ => None,
        }
    }

    pub fn as_arr(&self) -> Option<&Vec<Json>> {
        match self {
            Json::Arr(a) => Some(a),
            _ => None,
        }
    }

    pub fn str_or(&self, key: &str, default: &str) -> String {
        self.get(key)
            .and_then(|v| v.as_str())
            .unwrap_or(default)
            .to_string()
    }

    pub fn parse(text: &str) -> Result<Json, String> {
        let mut p = Parser {
            bytes: text.as_bytes(),
            pos: 0,
        };
        p.skip_ws();
        let value = p.value()?;
        p.skip_ws();
        if p.pos != p.bytes.len() {
            return Err(format!("лишние данные в JSON на позиции {}", p.pos));
        }
        Ok(value)
    }

    pub fn to_string_pretty(&self) -> String {
        let mut out = String::new();
        write_json(self, 0, &mut out);
        out
    }
}

fn write_json(value: &Json, indent: usize, out: &mut String) {
    let pad = "  ".repeat(indent);
    let pad_in = "  ".repeat(indent + 1);
    match value {
        Json::Null => out.push_str("null"),
        Json::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        Json::Num(n) => {
            if n.fract() == 0.0 && n.abs() < 1e15 {
                out.push_str(&format!("{}", *n as i64));
            } else {
                out.push_str(&format!("{n}"));
            }
        }
        Json::Str(s) => out.push_str(&quote(s)),
        Json::Arr(items) => {
            if items.is_empty() {
                out.push_str("[]");
                return;
            }
            out.push_str("[\n");
            for (i, item) in items.iter().enumerate() {
                out.push_str(&pad_in);
                write_json(item, indent + 1, out);
                if i + 1 < items.len() {
                    out.push(',');
                }
                out.push('\n');
            }
            out.push_str(&pad);
            out.push(']');
        }
        Json::Obj(map) => {
            if map.is_empty() {
                out.push_str("{}");
                return;
            }
            out.push_str("{\n");
            let len = map.len();
            for (i, (k, v)) in map.iter().enumerate() {
                out.push_str(&pad_in);
                out.push_str(&quote(k));
                out.push_str(": ");
                write_json(v, indent + 1, out);
                if i + 1 < len {
                    out.push(',');
                }
                out.push('\n');
            }
            out.push_str(&pad);
            out.push('}');
        }
    }
}

pub fn quote(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for ch in s.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

struct Parser<'a> {
    bytes: &'a [u8],
    pos: usize,
}

impl<'a> Parser<'a> {
    fn peek(&self) -> Option<u8> {
        self.bytes.get(self.pos).copied()
    }

    fn bump(&mut self) -> Option<u8> {
        let b = self.peek();
        if b.is_some() {
            self.pos += 1;
        }
        b
    }

    fn skip_ws(&mut self) {
        while let Some(b) = self.peek() {
            if b == b' ' || b == b'\t' || b == b'\n' || b == b'\r' {
                self.pos += 1;
            } else {
                break;
            }
        }
    }

    fn expect(&mut self, ch: u8) -> Result<(), String> {
        if self.bump() == Some(ch) {
            Ok(())
        } else {
            Err(format!("ожидался '{}' на позиции {}", ch as char, self.pos))
        }
    }

    fn value(&mut self) -> Result<Json, String> {
        self.skip_ws();
        match self.peek() {
            Some(b'{') => self.object(),
            Some(b'[') => self.array(),
            Some(b'"') => Ok(Json::Str(self.string()?)),
            Some(b't') => {
                self.literal("true")?;
                Ok(Json::Bool(true))
            }
            Some(b'f') => {
                self.literal("false")?;
                Ok(Json::Bool(false))
            }
            Some(b'n') => {
                self.literal("null")?;
                Ok(Json::Null)
            }
            Some(_) => self.number(),
            None => Err("неожиданный конец JSON".into()),
        }
    }

    fn literal(&mut self, word: &str) -> Result<(), String> {
        if self.bytes[self.pos..].starts_with(word.as_bytes()) {
            self.pos += word.len();
            Ok(())
        } else {
            Err(format!("ожидалось '{word}' на позиции {}", self.pos))
        }
    }

    fn object(&mut self) -> Result<Json, String> {
        self.expect(b'{')?;
        let mut map = BTreeMap::new();
        self.skip_ws();
        if self.peek() == Some(b'}') {
            self.pos += 1;
            return Ok(Json::Obj(map));
        }
        loop {
            self.skip_ws();
            let key = self.string()?;
            self.skip_ws();
            self.expect(b':')?;
            let value = self.value()?;
            map.insert(key, value);
            self.skip_ws();
            match self.bump() {
                Some(b',') => continue,
                Some(b'}') => return Ok(Json::Obj(map)),
                _ => return Err(format!("ожидалась ',' или '}}' на {}", self.pos)),
            }
        }
    }

    fn array(&mut self) -> Result<Json, String> {
        self.expect(b'[')?;
        let mut items = Vec::new();
        self.skip_ws();
        if self.peek() == Some(b']') {
            self.pos += 1;
            return Ok(Json::Arr(items));
        }
        loop {
            items.push(self.value()?);
            self.skip_ws();
            match self.bump() {
                Some(b',') => continue,
                Some(b']') => return Ok(Json::Arr(items)),
                _ => return Err(format!("ожидалась ',' или ']' на {}", self.pos)),
            }
        }
    }

    fn string(&mut self) -> Result<String, String> {
        self.expect(b'"')?;
        let mut out = String::new();
        loop {
            match self.bump() {
                None => return Err("незакрытая строка".into()),
                Some(b'"') => return Ok(out),
                Some(b'\\') => match self.bump() {
                    Some(b'"') => out.push('"'),
                    Some(b'\\') => out.push('\\'),
                    Some(b'/') => out.push('/'),
                    Some(b'n') => out.push('\n'),
                    Some(b'r') => out.push('\r'),
                    Some(b't') => out.push('\t'),
                    Some(b'b') => out.push('\u{8}'),
                    Some(b'f') => out.push('\u{c}'),
                    Some(b'u') => {
                        let hex = self.take(4)?;
                        let code = u32::from_str_radix(&hex, 16)
                            .map_err(|_| "плохой \\u escape".to_string())?;
                        // суррогатные пары
                        if (0xD800..0xDC00).contains(&code) {
                            if self.bytes[self.pos..].starts_with(b"\\u") {
                                self.pos += 2;
                                let low_hex = self.take(4)?;
                                let low = u32::from_str_radix(&low_hex, 16)
                                    .map_err(|_| "плохой \\u escape".to_string())?;
                                let ch = 0x10000
                                    + ((code - 0xD800) << 10)
                                    + (low.wrapping_sub(0xDC00) & 0x3FF);
                                out.push(char::from_u32(ch).unwrap_or('?'));
                            }
                        } else {
                            out.push(char::from_u32(code).unwrap_or('?'));
                        }
                    }
                    _ => return Err("плохой escape".into()),
                },
                Some(b) if b < 0x80 => out.push(b as char),
                Some(_) => {
                    // многобайтовый UTF-8: откатываемся на один байт и режем срез
                    self.pos -= 1;
                    let start = self.pos;
                    let width = utf8_width(self.bytes[start]).unwrap_or(1);
                    let end = (start + width).min(self.bytes.len());
                    out.push_str(
                        std::str::from_utf8(&self.bytes[start..end])
                            .map_err(|_| "битый UTF-8".to_string())?,
                    );
                    self.pos = end;
                }
            }
        }
    }

    fn take(&mut self, n: usize) -> Result<String, String> {
        if self.pos + n > self.bytes.len() {
            return Err("неожиданный конец строки".into());
        }
        let s = std::str::from_utf8(&self.bytes[self.pos..self.pos + n])
            .map_err(|_| "битый escape".to_string())?
            .to_string();
        self.pos += n;
        Ok(s)
    }

    fn number(&mut self) -> Result<Json, String> {
        let start = self.pos;
        if self.peek() == Some(b'-') {
            self.pos += 1;
        }
        while let Some(b) = self.peek() {
            if b.is_ascii_digit() || b == b'.' || b == b'e' || b == b'E'
                || b == b'+' || b == b'-'
            {
                self.pos += 1;
            } else {
                break;
            }
        }
        let text = std::str::from_utf8(&self.bytes[start..self.pos])
            .map_err(|_| "битое число".to_string())?;
        text.parse::<f64>()
            .map(Json::Num)
            .map_err(|_| format!("плохое число '{text}'"))
    }
}

fn utf8_width(b: u8) -> Option<usize> {
    match b {
        0x00..=0x7F => Some(1),
        0xC0..=0xDF => Some(2),
        0xE0..=0xEF => Some(3),
        0xF0..=0xF7 => Some(4),
        _ => None,
    }
}

// ---------------------------------------------------------------- конфиг
#[derive(Debug, Clone, PartialEq)]
pub struct Binding {
    pub button: String,
    pub action: String,
    /// Для action = "wheel": -5..-1 / 1..5 строк; для "media"/"launch" — аргумент
    pub arg: Option<String>,
}

#[derive(Debug, Clone)]
pub struct Config {
    pub bindings: Vec<Binding>,
    pub cursor_speed: Option<u32>,
    pub scroll_lines: Option<u32>,
    pub hscroll_chars: Option<u32>,
    pub profile: String,
    pub path: PathBuf,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            bindings: Vec::new(),
            cursor_speed: None,
            scroll_lines: None,
            hscroll_chars: None,
            profile: "auto".into(),
            path: default_config_path(),
        }
    }
}

pub fn default_config_path() -> PathBuf {
    let base = std::env::var("APPDATA")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from("."));
    base.join("gmouse").join("config.json")
}

impl Config {
    pub fn load_or_default(path: &Path) -> (Config, Option<String>) {
        match fs::read_to_string(path) {
            Ok(text) => match Config::from_json(&text) {
                Ok(mut cfg) => {
                    cfg.path = path.to_path_buf();
                    (cfg, None)
                }
                Err(err) => {
                    let mut cfg = Config::default();
                    cfg.path = path.to_path_buf();
                    (cfg, Some(format!("конфиг {} не разобран: {err}", path.display())))
                }
            },
            Err(_) => {
                let mut cfg = Config::default();
                cfg.path = path.to_path_buf();
                (cfg, None)
            }
        }
    }

    pub fn from_json(text: &str) -> Result<Config, String> {
        let root = Json::parse(text)?;
        let mut cfg = Config::default();
        cfg.profile = root.str_or("profile", "auto");
        cfg.cursor_speed = root.get("cursor_speed").and_then(|v| v.as_int())
            .and_then(|v| u32::try_from(v).ok());
        cfg.scroll_lines = root.get("scroll_lines").and_then(|v| v.as_int())
            .and_then(|v| u32::try_from(v).ok());
        cfg.hscroll_chars = root.get("hscroll_chars").and_then(|v| v.as_int())
            .and_then(|v| u32::try_from(v).ok());
        if let Some(items) = root.get("bindings").and_then(|v| v.as_arr()) {
            for item in items {
                let button = item.str_or("button", "");
                let action = item.str_or("action", "");
                if button.is_empty() || action.is_empty() {
                    return Err("у привязки нужны поля button и action".into());
                }
                let arg = item.get("arg").and_then(|v| match v {
                    Json::Str(s) => Some(s.clone()),
                    Json::Num(n) => Some(format!("{}", *n as i64)),
                    _ => None,
                });
                cfg.bindings.push(Binding { button, action, arg });
            }
        }
        Ok(cfg)
    }

    pub fn to_json(&self) -> Json {
        let mut root = BTreeMap::new();
        root.insert("profile".into(), Json::Str(self.profile.clone()));
        if let Some(v) = self.cursor_speed {
            root.insert("cursor_speed".into(), Json::Num(v as f64));
        }
        if let Some(v) = self.scroll_lines {
            root.insert("scroll_lines".into(), Json::Num(v as f64));
        }
        if let Some(v) = self.hscroll_chars {
            root.insert("hscroll_chars".into(), Json::Num(v as f64));
        }
        let bindings = self
            .bindings
            .iter()
            .map(|b| {
                let mut item = BTreeMap::new();
                item.insert("button".into(), Json::Str(b.button.clone()));
                item.insert("action".into(), Json::Str(b.action.clone()));
                if let Some(arg) = &b.arg {
                    let parsed = arg.parse::<f64>();
                    item.insert(
                        "arg".into(),
                        match parsed {
                            Ok(n) if !arg.contains('.') => Json::Num(n),
                            _ => Json::Str(arg.clone()),
                        },
                    );
                }
                Json::Obj(item)
            })
            .collect();
        root.insert("bindings".into(), Json::Arr(bindings));
        Json::Obj(root)
    }

    pub fn save(&self) -> std::io::Result<PathBuf> {
        if let Some(dir) = self.path.parent() {
            fs::create_dir_all(dir)?;
        }
        let mut text = self.to_json().to_string_pretty();
        text.push('\n');
        // атомарная запись: сначала во временный файл
        let tmp = self.path.with_extension("json.tmp");
        fs::write(&tmp, text)?;
        fs::rename(&tmp, &self.path)?;
        Ok(self.path.clone())
    }
}

// ---------------------------------------------------------------- профили
/// Профиль устройства: подсказки по DPI и режимам для конкретной мыши.
#[derive(Debug, Clone)]
pub struct Profile {
    pub model: String,
    pub pids: Vec<u16>,
    pub name_contains: String,
    pub dpi_min: u32,
    pub dpi_max: u32,
    pub dpi_step: u32,
    /// Код активации vendor-протокола DPI (если известен для модели)
    pub dpi_protocol: Option<DpiProtocol>,
}

/// Найденный vendor-протокол DPI (то, что печатает `gmouse identify`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DpiProtocol {
    pub command_id: u8,
    pub sub_id: u8,
    pub offset: usize,
    /// "feature" или "output"
    pub transport: &'static str,
}

impl Profile {
    pub fn generic() -> Profile {
        Profile {
            model: "Универсальная мышь (Windows)".into(),
            pids: Vec::new(),
            name_contains: String::new(),
            dpi_min: 200,
            dpi_max: 16000,
            dpi_step: 50,
            dpi_protocol: None,
        }
    }

    pub fn matches(&self, product: &str, pid: u16) -> bool {
        if !self.pids.is_empty() && self.pids.contains(&pid) {
            return true;
        }
        if !self.name_contains.is_empty() {
            let needle = self.name_contains.to_lowercase();
            return product.to_lowercase().contains(&needle);
        }
        false
    }

    /** Найти профиль по имени устройства и PID среди встроенных. */
    pub fn find(product: &str, pid: u16) -> Profile {
        builtin_profiles()
            .into_iter()
            .find(|p| p.matches(product, pid))
            .unwrap_or_else(Profile::generic)
    }
}

/// Встроенные профили. Расширяются добавлением JSON в profiles/ —
/// см. `load_profiles_dir`.
pub fn builtin_profiles() -> Vec<Profile> {
    vec![
        Profile {
            model: "Xenta / Eastern Times 2.4G Wireless (DS2511 и аналоги)".into(),
            pids: vec![0x2124, 0x2511],
            name_contains: String::new(),
            dpi_min: 800,
            dpi_max: 3200,
            dpi_step: 400,
            dpi_protocol: None,
        },
        Profile {
            model: "Generic HID Gaming Mouse (China Resource Semico)".into(),
            pids: Vec::new(),
            name_contains: "1a2c".into(),
            dpi_min: 800,
            dpi_max: 6400,
            dpi_step: 400,
            dpi_protocol: None,
        },
    ]
}

pub fn load_profiles_dir(dir: &Path) -> Vec<Profile> {
    let mut out = Vec::new();
    let Ok(entries) = fs::read_dir(dir) else {
        return out;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.extension().and_then(|e| e.to_str()) != Some("json") {
            continue;
        }
        let Ok(text) = fs::read_to_string(&path) else {
            continue;
        };
        let Ok(json) = Json::parse(&text) else {
            continue;
        };
        out.push(Profile {
            model: json.str_or("model", "профиль"),
            pids: json
                .get("pids")
                .and_then(|v| v.as_arr())
                .map(|items| {
                    items
                        .iter()
                        .filter_map(|v| match v {
                            Json::Str(s) => u16::from_str_radix(
                                s.trim_start_matches("0x").trim_start_matches("0X"),
                                16,
                            )
                            .ok(),
                            Json::Num(n) => Some(*n as u16),
                            _ => None,
                        })
                        .collect()
                })
                .unwrap_or_default(),
            name_contains: json.str_or("name_contains", ""),
            dpi_min: json.get("dpi_min").and_then(|v| v.as_int()).unwrap_or(200)
                as u32,
            dpi_max: json.get("dpi_max").and_then(|v| v.as_int()).unwrap_or(16000)
                as u32,
            dpi_step: json.get("dpi_step").and_then(|v| v.as_int()).unwrap_or(50)
                as u32,
            dpi_protocol: json.get("dpi_protocol").and_then(|v| {
                let command_id = v.get("command_id")?.as_int()? as u8;
                let sub_id = v.get("sub_id").and_then(|x| x.as_int()).unwrap_or(0) as u8;
                let offset = v.get("offset").and_then(|x| x.as_int()).unwrap_or(2) as usize;
                let transport = match v.str_or("transport", "feature").as_str() {
                    "output" | "out" => "output",
                    _ => "feature",
                };
                Some(DpiProtocol {
                    command_id,
                    sub_id,
                    offset,
                    transport,
                })
            }),
        });
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_config() {
        let text = r#"{
            "profile": "auto",
            "cursor_speed": 12,
            "scroll_lines": 3,
            "bindings": [
                {"button": "x1", "action": "double_click"},
                {"button": "wheel_left", "action": "wheel", "arg": 3},
                {"button": "middle", "action": "hotkey", "arg": "ctrl+shift+esc"}
            ]
        }"#;
        let cfg = Config::from_json(text).unwrap();
        assert_eq!(cfg.cursor_speed, Some(12));
        assert_eq!(cfg.bindings.len(), 3);
        assert_eq!(cfg.bindings[1].arg.as_deref(), Some("3"));
    }

    #[test]
    fn roundtrip_config() {
        let mut cfg = Config::default();
        cfg.cursor_speed = Some(10);
        cfg.bindings.push(Binding {
            button: "x2".into(),
            action: "wheel".into(),
            arg: Some("-3".into()),
        });
        let text = cfg.to_json().to_string_pretty();
        let back = Config::from_json(&text).unwrap();
        assert_eq!(back.cursor_speed, Some(10));
        assert_eq!(back.bindings, cfg.bindings);
    }

    #[test]
    fn json_handles_unicode_and_escapes() {
        let v = Json::parse(r#"{"a": "мышь \"тест\"", "b": [1, 2.5, true]}"#)
            .unwrap();
        assert_eq!(v.get("a").unwrap().as_str().unwrap(), "мышь \"тест\"");
        assert_eq!(v.get("b").unwrap().as_arr().unwrap().len(), 3);
    }

    #[test]
    fn json_rejects_garbage() {
        assert!(Json::parse("{a: 1}").is_err());
        assert!(Json::parse("[1,2").is_err());
    }

    #[test]
    fn profile_reads_dpi_protocol() {
        let json = Json::parse(
            r#"{"model": "X", "dpi_protocol": {"command_id": 7, "sub_id": 5, "offset": 2, "transport": "feature"}}"#,
        )
        .unwrap();
        let value = json.get("dpi_protocol").unwrap();
        assert_eq!(value.get("command_id").unwrap().as_int(), Some(7));
        assert_eq!(value.str_or("transport", "feature"), "feature");
    }

    #[test]
    fn profile_matches_pid_and_name() {
        let p = Profile {
            model: "t".into(),
            pids: vec![0x2124],
            name_contains: String::new(),
            dpi_min: 1,
            dpi_max: 2,
            dpi_step: 1,
            dpi_protocol: None,
        };
        assert!(p.matches("anything", 0x2124));
        assert!(!p.matches("anything", 0x1111));
    }
}
