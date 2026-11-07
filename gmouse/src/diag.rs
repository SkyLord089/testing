//! Диагностика: что за мышь, какие у неё интерфейсы, что летит в репортах.
//!
//! `gmouse diag` собирает отчёт в JSON — его достаточно, чтобы дописать
//! профиль и (если нужно) протокол DPI под конкретную модель.

use crate::config::Json;
use crate::hid;
use crate::settings::SystemMouse;
use std::collections::BTreeMap;
use std::io::{self, Write};
use std::time::{Duration, Instant};

pub fn os_version() -> String {
    // Достаём версию из реестра через `cmd /c ver` не нужно: RtlGetVersion
    // объявлять лень, а точности «Windows 11 build 22631» хватает для отчёта.
    #[link(name = "ntdll")]
    extern "system" {
        fn RtlGetVersion(info: *mut OsVersionInfo) -> i32;
    }
    #[repr(C)]
    struct OsVersionInfo {
        size: u32,
        major: u32,
        minor: u32,
        build: u32,
        platform: u32,
        csd: [u16; 128],
    }
    let mut info = OsVersionInfo {
        size: std::mem::size_of::<OsVersionInfo>() as u32,
        major: 0,
        minor: 0,
        build: 0,
        platform: 0,
        csd: [0u16; 128],
    };
    unsafe {
        RtlGetVersion(&mut info);
    }
    let name = if info.major >= 10 && info.build >= 22000 {
        "Windows 11"
    } else if info.major >= 10 {
        "Windows 10"
    } else {
        "Windows"
    };
    format!("{name} {}.{} build {}", info.major, info.minor, info.build)
}

fn mouse_devices(devices: &[hid::HidInfo]) -> Vec<&hid::HidInfo> {
    devices.iter().filter(|d| d.is_mouse()).collect()
}

/// Краткая сводка: список мышей, их VID/PID и служебные интерфейсы.
pub fn summary(devices: &[hid::HidInfo]) -> String {
    let mut out = String::new();
    let mice = mouse_devices(devices);
    if mice.is_empty() {
        out.push_str("Мыши не найдены.\n");
        return out;
    }
    // группируем по VID:PID
    let mut groups: BTreeMap<String, Vec<&hid::HidInfo>> = BTreeMap::new();
    for mouse in mice {
        groups.entry(mouse.vid_pid()).or_default().push(mouse);
    }
    for (vid_pid, items) in &groups {
        let name: &str = items
            .iter()
            .map(|d| d.product.as_str())
            .find(|p| !p.is_empty())
            .unwrap_or("(без имени)");
        out.push_str(&format!("{vid_pid}  {name}\n"));
        for item in items {
            out.push_str(&format!(
                "    {:<38} usage=0x{:02X}/0x{:02X} in={} out={} feature={}\n",
                truncate(&item.path, 38),
                item.usage_page,
                item.usage,
                item.input_len,
                item.output_len,
                item.feature_len
            ));
        }
        let services: Vec<_> = devices
            .iter()
            .filter(|d| d.parent == items[0].parent && d.is_vendor_defined())
            .collect();
        if services.is_empty() {
            out.push_str("    служебный vendor-интерфейс (0xFF00): не найден\n");
        } else {
            for svc in services {
                out.push_str(&format!(
                    "    служебный интерфейс: usage=0x{:02X}/0x{:02X} \
                     in={} out={} feature={}\n",
                    svc.usage_page, svc.usage, svc.input_len, svc.output_len,
                    svc.feature_len
                ));
            }
        }
    }
    out
}

fn truncate(text: &str, max: usize) -> String {
    if text.chars().count() <= max {
        return text.to_string();
    }
    let head: String = text.chars().take(max.saturating_sub(1)).collect();
    format!("{head}…")
}

/// Сырой дамп входных репортов: печатаем только изменяющиеся байты.
/// Помогает понять, где в репорте лежат кнопки/колесо/счётчики.
pub fn watch_reports(path: &str, seconds: u64) -> Result<String, String> {
    let Some(handle) = hid::open_rw(path) else {
        return Err(format!(
            "не удалось открыть {path}: нужно запустить от администратора \
             или закрыть программу, которая уже держит устройство"
        ));
    };
    let info = hid::enumerate()
        .into_iter()
        .find(|d| d.path == path)
        .ok_or_else(|| "устройство пропало".to_string())?;
    let len = info.input_len as usize;
    if len == 0 {
        hid::close(handle);
        return Err("у интерфейса нет входных репортов".into());
    }

    println!(
        "Читаю репорты ({} байт) {} с. Двигайте мышью и жмите кнопки.",
        len, seconds
    );
    println!("Печатаются только репорты, у которых изменились байты.\n");

    let mut out = String::from("ts_ms report\n");
    let start = Instant::now();
    let mut prev: Option<Vec<u8>> = None;
    let mut count = 0usize;
    while start.elapsed() < Duration::from_secs(seconds) {
        let Some(report) = hid::read_input(handle, len) else {
            break;
        };
        if let Some(prev) = &prev {
            if prev == &report {
                continue;
            }
        }
        let line = format!("{:>6} {}\n", start.elapsed().as_millis(), hex(&report));
        print!("{line}");
        let _ = io::stdout().flush();
        out.push_str(&line);
        prev = Some(report);
        count += 1;
    }
    hid::close(handle);
    println!("\nВсего репортов: {count}");
    Ok(out)
}

pub fn hex(bytes: &[u8]) -> String {
    bytes
        .iter()
        .map(|b| format!("{b:02X}"))
        .collect::<Vec<_>>()
        .join(" ")
}

pub fn build_report(devices: &[hid::HidInfo]) -> Json {
    let settings = SystemMouse::read();
    let mut root = BTreeMap::new();
    root.insert("tool".into(), Json::Str("gmouse".into()));
    root.insert(
        "version".into(),
        Json::Str(env!("CARGO_PKG_VERSION").into()),
    );
    root.insert("os".into(), Json::Str(os_version()));
    root.insert(
        "arch".into(),
        Json::Str(std::env::consts::ARCH.into()),
    );

    let mut system = BTreeMap::new();
    system.insert(
        "cursor_speed".into(),
        Json::Num(settings.cursor_speed as f64),
    );
    system.insert(
        "speed_multiplier".into(),
        Json::Num(settings.speed_multiplier()),
    );
    system.insert(
        "scroll_lines".into(),
        Json::Num(settings.scroll_lines as f64),
    );
    system.insert(
        "hscroll_chars".into(),
        Json::Num(settings.hscroll_chars as f64),
    );
    system.insert(
        "buttons_swapped".into(),
        Json::Bool(settings.buttons_swapped),
    );
    root.insert("system".into(), Json::Obj(system));

    let mut list = Vec::new();
    for device in devices {
        let mut item = BTreeMap::new();
        item.insert("path".into(), Json::Str(device.path.clone()));
        item.insert("vid".into(), Json::Str(format!("0x{:04X}", device.vendor_id)));
        item.insert("pid".into(), Json::Str(format!("0x{:04X}", device.product_id)));
        item.insert("product".into(), Json::Str(device.product.clone()));
        item.insert(
            "manufacturer".into(),
            Json::Str(device.manufacturer.clone()),
        );
        item.insert("serial".into(), Json::Str(device.serial.clone()));
        item.insert("kind".into(), Json::Str(device.kind().into()));
        item.insert("usage_page".into(), Json::Num(device.usage_page as f64));
        item.insert("usage".into(), Json::Num(device.usage as f64));
        item.insert("input_len".into(), Json::Num(device.input_len as f64));
        item.insert("output_len".into(), Json::Num(device.output_len as f64));
        item.insert("feature_len".into(), Json::Num(device.feature_len as f64));
        list.push(Json::Obj(item));
    }
    root.insert("devices".into(), Json::Arr(list));
    Json::Obj(root)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn device(name: &str, page: u16, usage: u16) -> hid::HidInfo {
        hid::HidInfo {
            path: format!("\\\\?\\hid#{name}"),
            vendor_id: 0x1D57,
            product_id: 0x2124,
            version: 0x0100,
            product: "USB Optical Mouse".into(),
            manufacturer: "Xenta".into(),
            serial: String::new(),
            usage_page: page,
            usage,
            input_len: 8,
            output_len: 0,
            feature_len: 9,
            parent: "\\\\?\\hid#parent".into(),
        }
    }

    #[test]
    fn hex_formats_pairs() {
        assert_eq!(hex(&[0x0A, 0xFF]), "0A FF");
    }

    #[test]
    fn summary_mentions_service_interface() {
        let devices = vec![device("mouse", 0x01, 0x02), device("svc", 0xFF00, 0x01)];
        let text = summary(&devices);
        assert!(text.contains("1D57:2124"));
        assert!(text.contains("служебный интерфейс"));
    }

    #[test]
    fn report_is_valid_json() {
        let devices = vec![device("mouse", 0x01, 0x02)];
        let text = build_report(&devices).to_string_pretty();
        let parsed = Json::parse(&text).unwrap();
        let arr = parsed.get("devices").unwrap().as_arr().unwrap();
        assert_eq!(arr.len(), 1);
        assert_eq!(
            arr[0].get("vid").unwrap().as_str().unwrap(),
            "0x1D57"
        );
    }

    #[test]
    fn truncate_keeps_length() {
        let short = truncate("abc", 10);
        assert_eq!(short, "abc");
        let long = truncate(&"x".repeat(30), 10);
        assert_eq!(long.chars().count(), 10);
    }
}
