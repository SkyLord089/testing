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
    if devices.is_empty() {
        out.push_str("HID-устройства не найдены.\n");
        return out;
    }

    // Группируем по физическому устройству (VID:PID + экземпляр), чтобы
    // показать все интерфейсы каждой мыши: мышь, клавиатуру, служебный.
    let mut groups: BTreeMap<String, Vec<&hid::HidInfo>> = BTreeMap::new();
    for device in devices {
        let key = format!(
            "{:04X}:{:04X} {}",
            device.vendor_id,
            device.product_id,
            &device.parent
        );
        groups.entry(key).or_default().push(device);
    }

    for (key, items) in &groups {
        let name = items
            .iter()
            .map(|d| d.product.as_str())
            .find(|p| !p.is_empty())
            .unwrap_or("(без имени)");
        let has_mouse = items.iter().any(|d| d.is_mouse());
        let has_service = items.iter().any(|d| hid::is_service_interface(d));
        let mark = if has_mouse { "МЫШЬ" } else { "     " };
        out.push_str(&format!("{mark} {key}  {name}\n"));
        for item in items {
            out.push_str(&format!(
                "      {:<10} usage=0x{:02X}/0x{:02X} in={} out={} feature={}\n",
                if item.is_mouse() {
                    "мышь"
                } else if item.usage_page == 0x01 && item.usage == 0x06 {
                    "клавиатура"
                } else if hid::is_service_interface(item) {
                    "служебный"
                } else {
                    "иное"
                },
                item.usage_page,
                item.usage,
                item.input_len,
                item.output_len,
                item.feature_len
            ));
        }
        if has_mouse && !has_service {
            out.push_str(
                "      ! служебного интерфейса нет: у этого устройства настройки\n\
                 \t(DPI, подсветка) не выведены в HID — их меняет только\n\
                 \tкнопка на корпусе или фирменная утилита.\n",
            );
        }
    }
    out
}

/// Подробная диагностика: сколько интерфейсов нашёл SetupAPI, какие пути,
/// что ответил CreateFileW, есть ли служебный интерфейс для настроек.
pub fn doctor() -> String {
    let mut out = String::new();
    out.push_str(&format!("ОС: {}\n", os_version()));

    let devices = hid::enumerate();
    let stats = hid::last_stats();
    out.push_str(&format!(
        "SetupAPI: интерфейсов={}, открыто={}, ошибок={}\n",
        stats.interfaces, stats.opened, stats.errors
    ));

    out.push_str("\nПервые интерфейсы (проверка доступа):\n");
    let mut shown = 0;
    for path in hid::interface_paths().into_iter().take(15) {
        let code = hid::probe_open(&path);
        let verdict = match code {
            0 => "открывается".to_string(),
            other => format!("код ошибки {other}"),
        };
        let short = if path.chars().count() > 90 {
            let head: String = path.chars().take(90).collect();
            format!("{head}…")
        } else {
            path.clone()
        };
        out.push_str(&format!("    [{verdict}] {short}\n"));
        shown += 1;
    }
    if shown == 0 {
        out.push_str("    (SetupAPI не вернул ни одного HID-интерфейса)\n");
    }

    out.push_str(&format!("\nРазобрано устройств: {}\n", devices.len()));
    for device in &devices {
        out.push_str(&format!(
            "    {} {:04X}:{:04X} {:<28} usage=0x{:02X}/0x{:02X} in={}\n",
            if device.is_mouse() { "МЫШЬ" } else { "     " },
            device.vendor_id,
            device.product_id,
            device.product,
            device.usage_page,
            device.usage,
            device.input_len
        ));
    }

    let services: Vec<_> = devices
        .iter()
        .filter(|d| hid::is_service_interface(d))
        .collect();
    out.push_str(&format!(
        "\nСлужебных интерфейсов (для настроек): {}\n",
        services.len()
    ));
    for service in services {
        out.push_str(&format!(
            "    usage=0x{:02X}/0x{:02X} in={} out={} feature={}  {}\n",
            service.usage_page,
            service.usage,
            service.input_len,
            service.output_len,
            service.feature_len,
            service.path
        ));
    }
    if devices.iter().any(|d| d.is_mouse()) && devices.iter().all(|d| !hid::is_service_interface(d))
    {
        out.push_str(
            "    Ни одного. Значит настройки через HID недоступны: DPI\n\
             \tпереключается кнопкой на корпусе, подсветка и макросы —\n\
             \tтолько фирменной утилитой производителя.\n",
        );
    }

    out.push_str("\nПолный список интерфейсов:\n");
    out.push_str(&full_dump(&devices));

    if devices.is_empty() || !devices.iter().any(|d| d.is_mouse()) {
        out.push_str(
            "\nМышь не найдена через HID. Что это значит:\n\
             * мышь подключена через RDP/виртуальную машину — HID-интерфейсы\n\
             \tвидны только внутри гостевой системы;\n\
             * это Bluetooth-мышь, и её HID-интерфейсы появляются только\n\
             \tпосле сопряжения (Параметры → Bluetooth);\n\
             * устройство отключено или спит.\n",
        );
    }
    out
}

/// Полный дамп всех интерфейсов — для отчёта разработчику.
pub fn full_dump(devices: &[hid::HidInfo]) -> String {
    let mut out = String::from("VID:PID    mi   usage      in  out feat  kind        path\n");
    for device in devices {
        out.push_str(&format!(
            "{:04X}:{:04X}  {:<4} 0x{:02X}/0x{:02X}  {:>3} {:>3} {:>4}  {:<11} {}\n",
            device.vendor_id,
            device.product_id,
            interface_number(&device.path),
            device.usage_page,
            device.usage,
            device.input_len,
            device.output_len,
            device.feature_len,
            device.kind(),
            device.path
        ));
    }
    out
}

/// Номер интерфейса из пути (`&mi_02` -> "02", иначе "-").
pub fn interface_number(path: &str) -> String {
    path.find("&mi_")
        .and_then(|index| path.get(index + 4..index + 6))
        .map(|s| s.to_string())
        .unwrap_or_else(|| "-".into())
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

/// Метка времени в миллисекундах от старта программы — для потока событий.
pub fn now_ms() -> String {
    #[link(name = "kernel32")]
    extern "system" {
        fn GetTickCount() -> u32;
    }
    let ticks = unsafe { GetTickCount() };
    format!("{:>8.3}s", ticks as f64 / 1000.0)
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
    fn doctor_returns_text() {
        // На CI и в песочнице устройств нет — важно, что функция не падает
        // и печатает версию ОС.
        let text = doctor();
        assert!(text.contains("ОС:"));
        assert!(text.contains("SetupAPI"));
    }

    #[test]
    fn hex_formats_pairs() {
        assert_eq!(hex(&[0x0A, 0xFF]), "0A FF");
    }

    #[test]
    fn summary_groups_and_marks_service_interface() {
        let mut mouse = device("mouse", 0x01, 0x02);
        mouse.parent = "vid_1d57&pid_2124".into();
        let mut service = device("svc", 0xFF00, 0x01);
        service.parent = "vid_1d57&pid_2124".into();
        service.feature_len = 9;
        let text = summary(&[mouse, service]);
        assert!(text.contains("1D57:2124"));
        assert!(text.contains("служебный"));
        assert!(text.contains("МЫШЬ"));
    }

    #[test]
    fn summary_warns_when_no_service_interface() {
        let mut mouse = device("mouse", 0x01, 0x02);
        mouse.parent = "vid_1a2c&pid_2511".into();
        let text = summary(&[mouse]);
        assert!(text.contains("служебного интерфейса нет"));
    }

    #[test]
    fn full_dump_lists_every_interface() {
        let devices = vec![device("a", 0x01, 0x02), device("b", 0xFF00, 0x01)];
        let text = full_dump(&devices);
        assert_eq!(text.lines().count(), 3);
        assert!(text.contains("0xFF00/0x01"));
    }

    #[test]
    fn interface_number_from_path() {
        assert_eq!(
            interface_number("\\\\?\\hid#vid_1d57&pid_2124&mi_02&col01#8&1c2da351&0&0000#{g}"),
            "02"
        );
        assert_eq!(interface_number("\\\\?\\hid#vid_1d57&pid_2124#8&1#0"), "-");
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
