//! gmouse — настройка любой мыши под Windows 11.
//!
//! Что делает:
//!   * переназначает кнопки (включая боковые X1/X2, среднюю, наклоны колеса);
//!   * меняет скорость курсора, строки прокрутки, ускорение;
//!   * показывает, что за мышь подключена (VID/PID/интерфейсы);
//!   * помогает найти vendor-протокол аппаратного DPI для «безымянных» мышей;
//!   * собирает отчёт для доработки профиля.
//!
//! Всё через Win32 API, без зависимостей и без установки драйверов.

#![cfg_attr(not(windows), allow(dead_code, unused_imports))]

#[cfg(not(windows))]
compile_error!(
    "gmouse работает только на Windows: это программа для Windows 11 \
     (Win32 API + SetupAPI/HID)."
);

mod actions;
mod config;
mod diag;
mod hid;
mod hook;
mod protocol;
mod settings;
mod win;

use config::{Config, Profile};
use std::collections::BTreeMap;
use std::io::{self, Write};
use std::path::PathBuf;

const HELP: &str = "\
gmouse — настройка мыши под Windows 11

Использование:
  gmouse list                     список мышей и их интерфейсов
  gmouse status                   текущие настройки Windows для мыши
  gmouse speed <1..20>            скорость курсора (как в Параметрах)
  gmouse scroll <строк>           строк на одно деление колеса
  gmouse accel on|off             ускорение указателя
  gmouse test                     показать имена кнопок (для конфига)
  gmouse run                      применить привязки (работает в фоне)
  gmouse bind <кнопка> <действие> [аргумент]
  gmouse unbind <кнопка>
  gmouse bindings                 показать сохранённые привязки
  gmouse preset <имя>             готовый набор привязок
  gmouse probe [dpi]              отправить известные команды DPI
  gmouse identify                 подобрать команду DPI интерактивно
  gmouse watch <секунды>          дамп входных репортов служебного интерфейса
  gmouse diag [-o файл]           отчёт о мыши в JSON
  gmouse profiles                 известные профили
  gmouse config-path              где лежит конфиг

Кнопки: left, right, middle, x1, x2, wheel_up, wheel_down,
        tilt_left, tilt_right
Действия: left, right, middle, x1, x2, double_click, triple_click,
          wheel +N, hscroll -N, hotkey ctrl+shift+esc,
          text ..., app notepad.exe, url https://..., none

Примеры:
  gmouse bind x1 double_click
  gmouse bind x2 hotkey ctrl+shift+esc
  gmouse bind tilt_left hscroll -3
  gmouse preset linux-back-forward
  gmouse run
";

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let code = match run(&args) {
        Ok(code) => code,
        Err(message) => {
            eprintln!("Ошибка: {message}");
            2
        }
    };
    std::process::exit(code);
}

/// config.json -> привязки, которые понимает hook.rs
fn cfg_bindings(cfg: &Config, overrides: &[(String, String, Option<String>)]) -> Vec<hook::Binding> {
    let mut list: Vec<hook::Binding> = cfg
        .bindings
        .iter()
        .map(|b| hook::Binding {
            button: b.button.clone(),
            action: b.action.clone(),
            arg: b.arg.clone(),
        })
        .collect();
    for (button, action, arg) in overrides {
        list.retain(|b| !same_button(&b.button, button));
        list.push(hook::Binding {
            button: button.clone(),
            action: action.clone(),
            arg: arg.clone(),
        });
    }
    list
}

fn run_with_bindings(bindings: &[hook::Binding], test_mode: bool) -> Result<i32, String> {
    if !test_mode && bindings.is_empty() {
        println!(
            "Привязок нет. Добавьте, например:\n  \
             gmouse bind x1 double_click\n  \
             gmouse bind x2 hotkey ctrl+left"
        );
    }
    hook::run(bindings, test_mode)
}

fn run(args: &[String]) -> Result<i32, String> {
    let command = args.first().map(String::as_str).unwrap_or("help");
    let (cfg, warning) = Config::load_or_default(&config::default_config_path());
    if let Some(warning) = warning {
        eprintln!("Предупреждение: {warning}");
    }

    match command {
        "help" | "-h" | "--help" | "?" => {
            print!("{HELP}");
            Ok(0)
        }
        "list" | "devices" => cmd_list(),
        "status" => cmd_status(),
        "speed" => {
            let value: u32 = args
                .get(1)
                .ok_or("укажите скорость: gmouse speed 10")?
                .parse()
                .map_err(|_| "скорость — число от 1 до 20")?;
            settings::SystemMouse::read().set_cursor_speed(value)?;
            let updated = settings::SystemMouse::read();
            println!(
                "Скорость курсора: {} (множитель {:.3})",
                updated.cursor_speed,
                updated.speed_multiplier()
            );
            Ok(0)
        }
        "scroll" => {
            let value: u32 = args
                .get(1)
                .ok_or("укажите число строк: gmouse scroll 3")?
                .parse()
                .map_err(|_| "строки — целое число")?;
            settings::SystemMouse::read().set_scroll_lines(value)?;
            println!("Строк на деление колеса: {value}");
            Ok(0)
        }
        "accel" => {
            let enable = match args.get(1).map(String::as_str) {
                Some("on") | Some("1") | Some("да") => true,
                Some("off") | Some("0") | Some("нет") => false,
                _ => return Err("укажите on или off".into()),
            };
            settings::SystemMouse::read().set_acceleration(enable)?;
            println!(
                "Ускорение указателя: {}",
                if enable { "включено" } else { "выключено" }
            );
            Ok(0)
        }
        "test" => {
            println!(
                "Режим теста: кнопки не переназначаются, только печатаются \
                 их имена.\nНажмите нужную кнопку и запишите имя в конфиг.\n"
            );
            run_with_bindings(&[], true)
        }
        "run" | "daemon" => run_with_bindings(&cfg_bindings(&cfg, &[]), false),
        "bind" => cmd_bind(cfg, args, false),
        "try" => cmd_bind(cfg, args, true),
        "unbind" => cmd_unbind(cfg, args),
        "bindings" => cmd_bindings(&cfg),
        "preset" => cmd_preset(cfg, args),
        "probe" => cmd_probe(args),
        "identify" => cmd_identify(args),
        "watch" => cmd_watch(args),
        "diag" => cmd_diag(args),
        "profiles" => cmd_profiles(),
        "config-path" => {
            println!("{}", config::default_config_path().display());
            Ok(0)
        }
        other => {
            eprintln!("Неизвестная команда: {other}\n");
            print!("{HELP}");
            Ok(2)
        }
    }
}

fn cmd_list() -> Result<i32, String> {
    let devices = hid::enumerate();
    if devices.is_empty() {
        println!("HID-устройства не найдены.");
        return Ok(1);
    }
    print!("{}", diag::summary(&devices));
    if let Some(mouse) = devices.iter().find(|d| d.is_mouse()) {
        let profile = Profile::find(&mouse.product, mouse.product_id);
        println!(
            "\nПрофиль: {}\nОжидаемый диапазон DPI: {}..{} шаг {}",
            profile.model, profile.dpi_min, profile.dpi_max, profile.dpi_step
        );
    }
    Ok(0)
}

fn cmd_status() -> Result<i32, String> {
    let settings = settings::SystemMouse::read();
    println!("ОС: {}", diag::os_version());
    println!(
        "Скорость курсора: {} (множитель {:.3})",
        settings.cursor_speed,
        settings.speed_multiplier()
    );
    println!("Строк на деление колеса: {}", settings.scroll_lines);
    println!(
        "Символов горизонтальной прокрутки: {}",
        settings.hscroll_chars
    );
    println!(
        "Кнопки поменяны местами: {}",
        if settings.buttons_swapped { "да" } else { "нет" }
    );
    println!(
        "Порог двойного клика: {}x{} px",
        settings.double_click_width, settings.double_click_height
    );
    for dpi in [800u32, 1200, 1600, 2400] {
        println!(
            "  при сенсоре {dpi} DPI эффективно ≈ {:.0} CPI",
            settings.effective_cpi(dpi)
        );
    }
    Ok(0)
}

fn cmd_bind(mut cfg: Config, args: &[String], temporary: bool) -> Result<i32, String> {
    let button = args
        .get(1)
        .ok_or("формат: gmouse bind <кнопка> <действие> [аргумент]")?
        .clone();
    let action = args
        .get(2)
        .ok_or("укажите действие, например double_click")?
        .clone();
    if let Err(message) = hook::Button::parse(&button) {
        eprintln!("{message}");
        return Err(format!(
            "допустимые кнопки: left, right, middle, x1, x2, wheel_up, \
             wheel_down, tilt_left, tilt_right.\nТочные имена для вашей мыши \
             покажет: gmouse test"
        ));
    }
    let arg = args.get(3).cloned();

    if temporary {
        // проверить здесь и сейчас, не записывая в конфиг
        let bindings = cfg_bindings(&cfg, &[(button.clone(), action.clone(), arg.clone())]);
        println!(
            "Пробую: {} -> {}{}. Ctrl+Alt+Q — остановить.",
            button,
            action,
            arg.map(|a| format!(" {a}")).unwrap_or_default()
        );
        return run_with_bindings(&bindings, false);
    }

    cfg.bindings.retain(|b| !same_button(&b.button, &button));
    cfg.bindings.push(config::Binding {
        button: button.clone(),
        action: action.clone(),
        arg: arg.clone(),
    });
    let path = cfg
        .save()
        .map_err(|e| format!("не удалось сохранить конфиг: {e}"))?;
    println!(
        "Сохранено: {} -> {}{}",
        button,
        action,
        arg.map(|a| format!(" {a}")).unwrap_or_default()
    );
    println!("Конфиг: {}", path.display());
    println!("Примените: gmouse run");
    Ok(0)
}

fn same_button(a: &str, b: &str) -> bool {
    match (hook::Button::parse(a), hook::Button::parse(b)) {
        (Ok(x), Ok(y)) => x == y,
        _ => a.eq_ignore_ascii_case(b),
    }
}

fn cmd_unbind(mut cfg: Config, args: &[String]) -> Result<i32, String> {
    let button = args.get(1).ok_or("gmouse unbind <кнопка>")?;
    let before = cfg.bindings.len();
    cfg.bindings.retain(|b| !same_button(&b.button, button));
    cfg.save()
        .map_err(|e| format!("не удалось сохранить: {e}"))?;
    if cfg.bindings.len() == before {
        println!("Привязки для '{button}' не было.");
    } else {
        println!("Привязка для '{button}' удалена.");
    }
    Ok(0)
}

fn cmd_bindings(cfg: &Config) -> Result<i32, String> {
    if cfg.bindings.is_empty() {
        println!("Привязок нет.");
        return Ok(0);
    }
    for binding in &cfg.bindings {
        println!(
            "{:<12} -> {}{}",
            binding.button,
            binding.action,
            binding
                .arg
                .as_deref()
                .map(|a| format!(" {a}"))
                .unwrap_or_default()
        );
    }
    Ok(0)
}

fn preset_bindings(name: &str) -> Option<Vec<config::Binding>> {
    let make = |button: &str, action: &str, arg: Option<&str>| config::Binding {
        button: button.into(),
        action: action.into(),
        arg: arg.map(str::to_string),
    };
    match name {
        // Как в Linux: назад/вперёд в браузере и проводнике
        "linux-back-forward" | "default" => Some(vec![
            make("x1", "hotkey", Some("alt+left")),
            make("x2", "hotkey", Some("alt+right")),
        ]),
        // Средняя кнопка = двойной клик, боковая = звук
        "double-click" => Some(vec![
            make("middle", "double_click", None),
            make("x1", "hotkey", Some("volumemute")),
        ]),
        // Менеджер задач и проводник
        "power-user" => Some(vec![
            make("x1", "hotkey", Some("ctrl+shift+esc")),
            make("x2", "app", Some("explorer.exe")),
            make("middle", "double_click", None),
        ]),
        // Горизонтальная прокрутка наклонами колеса
        "tilt-scroll" => Some(vec![
            make("tilt_left", "hscroll", Some("-3")),
            make("tilt_right", "hscroll", Some("3")),
        ]),
        // Всё выключить
        "clear" => Some(Vec::new()),
        _ => None,
    }
}

fn cmd_preset(mut cfg: Config, args: &[String]) -> Result<i32, String> {
    let name = args.get(1).ok_or(
        "укажите набор: linux-back-forward | double-click | power-user | \
         tilt-scroll | clear",
    )?;
    let Some(bindings) = preset_bindings(name) else {
        return Err(format!(
            "неизвестный набор '{name}'. Доступны: linux-back-forward, \
             double-click, power-user, tilt-scroll, clear"
        ));
    };
    cfg.bindings = bindings;
    let path = cfg
        .save()
        .map_err(|e| format!("ошибка сохранения: {e}"))?;
    println!("Набор '{name}' записан в {}", path.display());
    cmd_bindings(&cfg)?;
    Ok(0)
}

fn find_service_interface() -> Result<(hid::HidInfo, win::HANDLE), String> {
    let devices = hid::enumerate();
    let mouse = devices
        .iter()
        .find(|d| d.is_mouse())
        .ok_or("мышь не найдена")?
        .clone();
    // служебный интерфейс того же физического устройства
    let service = devices
        .iter()
        .find(|d| d.parent == mouse.parent && d.is_vendor_defined())
        .or_else(|| devices.iter().find(|d| d.parent == mouse.parent))
        .ok_or("не найден служебный интерфейс — у этой мыши нет vendor-HID")?
        .clone();
    let handle = hid::open_rw(&service.path).ok_or_else(|| {
        format!(
            "не удалось открыть {}: запустите gmouse от администратора",
            service.path
        )
    })?;
    Ok((service, handle))
}

fn cmd_probe(args: &[String]) -> Result<i32, String> {
    let dpi: u32 = args
        .get(1)
        .map(|s| s.parse())
        .transpose()
        .map_err(|_| "DPI — число, например 1600")?
        .unwrap_or(1600);
    let (info, handle) = find_service_interface()?;
    println!("Служебный интерфейс: {} ({})", info.path, info.vid_pid());
    println!("Отправляю DPI = {dpi} всеми известными наборами команд.\n");
    let target = protocol::Target { handle };
    let results = protocol::probe_all(&target, dpi);
    let mut answered = 0;
    for result in &results {
        let mark = match &result.response {
            Some(reply) => {
                answered += 1;
                format!("ответ: {}", protocol::hex(reply))
            }
            None => "нет ответа".to_string(),
        };
        println!("  {:<34} {mark}", result.candidate.describe());
    }
    hid::close(handle);
    println!("\nОтветили: {answered} из {}", results.len());
    if answered == 0 {
        println!(
            "Ни одна команда не принята. Значит DPI у вашей мыши, скорее \n\
             всего, переключается кнопкой на корпусе или не настраивается."
        );
    } else {
        println!(
            "Теперь проверьте скорость курсора. Какой набор подействовал — \n\
             уточнит команда: gmouse identify"
        );
    }
    Ok(0)
}

fn cmd_identify(args: &[String]) -> Result<i32, String> {
    let low: u32 = args
        .get(1)
        .map(|s| s.parse())
        .transpose()
        .map_err(|_| "низкий DPI — число")?
        .unwrap_or(400);
    let high: u32 = args
        .get(2)
        .map(|s| s.parse())
        .transpose()
        .map_err(|_| "высокий DPI — число")?
        .unwrap_or(1600);
    let (info, handle) = find_service_interface()?;
    println!("Устройство: {} ({})", info.product, info.vid_pid());
    println!(
        "Проверяем команды: сначала DPI={low} (курсор заметно медленнее), \
         затем DPI={high}."
    );
    let target = protocol::Target { handle };
    let found = protocol::identify(&target, low, high);
    hid::close(handle);
    match found {
        Some(candidate) => {
            println!("Готово: {}", candidate.describe());
            println!(
                "Допишите в профиль profiles/{}.json: \"dpi_protocol\": {}, \
                 \"command_id\": {}, \"sub_id\": {}, \"offset\": {}, \
                 \"transport\": \"{}\"",
                info.vid_pid().replace(':', "_").to_lowercase(),
                candidate.command_id,
                candidate.command_id,
                candidate.sub_id,
                candidate.offset,
                candidate.transport.name()
            );
            Ok(0)
        }
        None => Ok(1),
    }
}

fn cmd_watch(args: &[String]) -> Result<i32, String> {
    let seconds: u64 = args
        .get(1)
        .map(|s| s.parse())
        .transpose()
        .map_err(|_| "секунды — число")?
        .unwrap_or(10);
    let (info, _) = find_service_interface()?;
    let text = diag::watch_reports(&info.path, seconds)?;
    let out = PathBuf::from(format!(
        "report_dump_{}.txt",
        info.vid_pid().replace(':', "_")
    ));
    std::fs::write(&out, text).map_err(|e| e.to_string())?;
    println!("Дамп сохранён: {}", out.display());
    Ok(0)
}

fn cmd_diag(args: &[String]) -> Result<i32, String> {
    let devices = hid::enumerate();
    let mut report = match diag::build_report(&devices) {
        config::Json::Obj(map) => map,
        _ => BTreeMap::new(),
    };
    if let Some(mouse) = devices.iter().find(|d| d.is_mouse()) {
        let profile = Profile::find(&mouse.product, mouse.product_id);
        let mut prof = BTreeMap::new();
        prof.insert("model".into(), config::Json::Str(profile.model.clone()));
        prof.insert("dpi_min".into(), config::Json::Num(profile.dpi_min as f64));
        prof.insert("dpi_max".into(), config::Json::Num(profile.dpi_max as f64));
        prof.insert(
            "dpi_step".into(),
            config::Json::Num(profile.dpi_step as f64),
        );
        report.insert("profile".into(), config::Json::Obj(prof));
    }
    let text = config::Json::Obj(report).to_string_pretty();
    let file = args
        .iter()
        .position(|a| a == "-o")
        .and_then(|i| args.get(i + 1).cloned())
        .unwrap_or_else(|| {
            let name = devices
                .iter()
                .find(|d| d.is_mouse())
                .map(|d| d.vid_pid().replace(':', "_"))
                .unwrap_or_else(|| "mouse".into());
            format!("gmouse_report_{name}.json")
        });
    std::fs::write(&file, &text)
        .map_err(|e| format!("не удалось записать отчёт: {e}"))?;
    print!("{}", diag::summary(&devices));
    println!("\nОтчёт сохранён: {file}");
    println!("Пришлите этот файл — по нему будет дописан профиль вашей мыши.");
    Ok(0)
}

fn cmd_profiles() -> Result<i32, String> {
    for profile in config::builtin_profiles() {
        println!(
            "{:<52} DPI {}..{}",
            profile.model, profile.dpi_min, profile.dpi_max
        );
    }
    let dir = std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.join("profiles")))
        .unwrap_or_else(|| PathBuf::from("profiles"));
    let extra = config::load_profiles_dir(&dir);
    if !extra.is_empty() {
        println!("\nИз {}:", dir.display());
        for profile in extra {
            println!(
                "  {:<50} DPI {}..{}",
                profile.model, profile.dpi_min, profile.dpi_max
            );
        }
    }
    Ok(0)
}

#[allow(dead_code)]
fn _unused_imports() {
    // io/Write пригодятся для интерактивных подсказок в будущем
    let _ = io::stdout().flush();
}
