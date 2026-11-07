//! Глобальные хуки: переназначение кнопок мыши в реальном времени.
//!
//! Хук `WH_MOUSE_LL` ставится на поток со своим циклом сообщений.
//! Событие можно «съесть» (вернуть 1), тогда оно не дойдёт до системы;
//! вместо него выполняется действие из конфига.
//!
//! Важно: синтезированные нами события несут метку G_MOUSE_MARK в
//! dwExtraInfo, поэтому хук их пропускает — цикла не будет.

use crate::actions;
use crate::win::*;
use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicIsize, Ordering};
use std::sync::Mutex;

pub const WM_MOUSEMOVE: u32 = 0x0200;
pub const WM_LBUTTONDOWN: u32 = 0x0201;
pub const WM_LBUTTONUP: u32 = 0x0202;
pub const WM_RBUTTONDOWN: u32 = 0x0204;
pub const WM_RBUTTONUP: u32 = 0x0205;
pub const WM_MBUTTONDOWN: u32 = 0x0207;
pub const WM_MBUTTONUP: u32 = 0x0208;
pub const WM_MOUSEWHEEL: u32 = 0x020A;
pub const WM_XBUTTONDOWN: u32 = 0x020B;
pub const WM_XBUTTONUP: u32 = 0x020C;
pub const WM_MOUSEHWHEEL: u32 = 0x020E;

pub const WM_KEYDOWN: u32 = 0x0100;
pub const WM_SYSKEYDOWN: u32 = 0x0104;
pub const WM_HOTKEY_STOP: u32 = 0x8000 + 1;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Button {
    X1,
    X2,
    Middle,
    WheelUp,
    WheelDown,
    TiltLeft,
    TiltRight,
    Left,
    Right,
}

impl Button {
    pub fn name(self) -> &'static str {
        match self {
            Button::X1 => "x1",
            Button::X2 => "x2",
            Button::Middle => "middle",
            Button::WheelUp => "wheel_up",
            Button::WheelDown => "wheel_down",
            Button::TiltLeft => "tilt_left",
            Button::TiltRight => "tilt_right",
            Button::Left => "left",
            Button::Right => "right",
        }
    }

    /// Разбор имени кнопки с комментарием для пользователя.
    pub fn parse(name: &str) -> Result<Button, String> {
        let normalized = name.trim().to_lowercase().replace('-', "_");
        Self::parse_exact(&normalized)
            .ok_or_else(|| format!("неизвестная кнопка '{name}'"))
    }

    /// Строгий разбор без сообщения об ошибке.
    pub fn parse_exact(name: &str) -> Option<Button> {
        let name = name.trim().to_lowercase().replace('-', "_");
        Some(match name.as_str() {
            "x1" | "back" | "side" | "browser_back" => Button::X1,
            "x2" | "forward" | "browser_forward" => Button::X2,
            "middle" | "mmb" | "wheel_click" => Button::Middle,
            "wheel_up" | "scroll_up" => Button::WheelUp,
            "wheel_down" | "scroll_down" => Button::WheelDown,
            "tilt_left" | "wheel_left" => Button::TiltLeft,
            "tilt_right" | "wheel_right" => Button::TiltRight,
            "left" | "lmb" => Button::Left,
            "right" | "rmb" => Button::Right,
            _ => return None,
        })
    }
}

/// Привязка «кнопка -> действие» в том виде, в каком её видит хук.
#[derive(Debug, Clone, PartialEq)]
pub struct Binding {
    pub button: String,
    pub action: String,
    pub arg: Option<String>,
}

pub struct State {
    pub bindings: HashMap<Button, Binding>,
    pub test_mode: bool,
    pub running: AtomicBool,
    pub thread_id: AtomicIsize,
    /// Какие кнопки сейчас нажаты и «съедены» — их отпускание тоже съедаем.
    pub swallowed: Mutex<Vec<Button>>,
}

static STATE: Mutex<Option<&'static State>> = Mutex::new(None);

fn state() -> Option<&'static State> {
    *STATE.lock().unwrap()
}

extern "system" fn mouse_proc(code: i32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    if code == HC_ACTION {
        let info = unsafe { &*(lparam as *const MSLLHOOKSTRUCT) };
        if info.dwExtraInfo == G_MOUSE_MARK {
            return unsafe { CallNextHookEx(0, code, wparam, lparam) };
        }
        let msg = wparam as u32;
        let high = (info.mouseData >> 16) as u16;

        // has_up = false у колеса: у прокрутки нет парного «отпускания»,
        // поэтому её нельзя запоминать в списке съеденных нажатий.
        let (button, is_down, has_up) = match msg {
            WM_XBUTTONDOWN => (
                if high == 1 { Button::X1 } else { Button::X2 },
                true,
                true,
            ),
            WM_XBUTTONUP => (
                if high == 1 { Button::X1 } else { Button::X2 },
                false,
                true,
            ),
            WM_MBUTTONDOWN => (Button::Middle, true, true),
            WM_MBUTTONUP => (Button::Middle, false, true),
            WM_LBUTTONDOWN => (Button::Left, true, true),
            WM_LBUTTONUP => (Button::Left, false, true),
            WM_RBUTTONDOWN => (Button::Right, true, true),
            WM_RBUTTONUP => (Button::Right, false, true),
            WM_MOUSEWHEEL => {
                let delta = (info.mouseData >> 16) as i16;
                (
                    if delta > 0 { Button::WheelUp } else { Button::WheelDown },
                    true,
                    false,
                )
            }
            WM_MOUSEHWHEEL => {
                let delta = (info.mouseData >> 16) as i16;
                (
                    if delta > 0 { Button::TiltRight } else { Button::TiltLeft },
                    true,
                    false,
                )
            }
            _ => return unsafe { CallNextHookEx(0, code, wparam, lparam) },
        };

        let Some(st) = state() else {
            return unsafe { CallNextHookEx(0, code, wparam, lparam) };
        };

        if st.test_mode {
            println!(
                "  [тест] {:<11} {}",
                button.name(),
                if is_down { "нажата" } else { "отпущена" }
            );
            return unsafe { CallNextHookEx(0, code, wparam, lparam) };
        }

        // Отпускание «съеденной» кнопки тоже съедаем, иначе она залипнет.
        if !is_down {
            let mut swallowed = st.swallowed.lock().unwrap();
            if swallowed.contains(&button) {
                swallowed.retain(|b| *b != button);
                return 1;
            }
            return unsafe { CallNextHookEx(0, code, wparam, lparam) };
        }

        let Some(binding) = st.bindings.get(&button).cloned() else {
            return unsafe { CallNextHookEx(0, code, wparam, lparam) };
        };

        // Блокировку не держим: действие может занять время (запуск программы).
        let performed = actions::perform(&binding.action, binding.arg.as_deref());
        if performed {
            if has_up {
                st.swallowed.lock().unwrap().push(button);
            }
            return 1; // нажатие не доходит до системы
        }
        eprintln!(
            "gmouse: действие '{}' для '{}' не выполнено",
            binding.action,
            button.name()
        );
    }
    unsafe { CallNextHookEx(0, code, wparam, lparam) }
}

extern "system" fn keyboard_proc(code: i32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    if code == HC_ACTION {
        let msg = wparam as u32;
        if msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN {
            let info = unsafe { &*(lparam as *const KBDLLHOOKSTRUCT) };
            let ctrl = unsafe { GetAsyncKeyState(0x11) < 0 };
            let alt = unsafe { GetAsyncKeyState(0x12) < 0 };
            // Ctrl+Alt+Q — аккуратный выход
            if ctrl && alt && info.vkCode == 'Q' as u32 {
                if let Some(st) = state() {
                    st.running.store(false, Ordering::SeqCst);
                    let tid = st.thread_id.load(Ordering::SeqCst) as u32;
                    unsafe {
                        PostThreadMessageW(tid, WM_QUIT, 0, 0);
                    }
                }
                return 1;
            }
        }
    }
    unsafe { CallNextHookEx(0, code, wparam, lparam) }
}

#[link(name = "user32")]
extern "system" {
    fn GetAsyncKeyState(vk: i32) -> i16;
}

/// Собрать карту привязок, отбросив некорректные имена кнопок.
pub fn build_bindings(bindings: &[Binding]) -> (HashMap<Button, Binding>, Vec<String>) {
    let mut map = HashMap::new();
    let mut warnings = Vec::new();
    for binding in bindings {
        match Button::parse(&binding.button) {
            Ok(button) => {
                map.insert(button, binding.clone());
            }
            Err(_) => warnings.push(format!(
                "неизвестная кнопка '{}' — пропущена (список: left, right, \
                 middle, x1, x2, wheel_up, wheel_down, tilt_left, tilt_right)",
                binding.button
            )),
        }
    }
    (map, warnings)
}

/// Запустить демон: хуки + цикл сообщений. Возвращает код выхода.
pub fn run(bindings_list: &[Binding], test_mode: bool) -> Result<i32, String> {
    let (bindings, warnings) = build_bindings(bindings_list);
    for warning in &warnings {
        eprintln!("gmouse: {warning}");
    }

    let state: &'static State = Box::leak(Box::new(State {
        bindings,
        test_mode,
        running: AtomicBool::new(true),
        thread_id: AtomicIsize::new(0),
        swallowed: Mutex::new(Vec::new()),
    }));
    *STATE.lock().unwrap() = Some(state);

    let (mouse_hook, keyboard_hook) = unsafe {
        let cur = GetCurrentThreadId();
        state.thread_id.store(cur as isize, Ordering::SeqCst);
        (
            SetWindowsHookExW(WH_MOUSE_LL, Some(mouse_proc), 0, 0),
            SetWindowsHookExW(WH_KEYBOARD_LL, Some(keyboard_proc), 0, 0),
        )
    };
    if mouse_hook == 0 {
        *STATE.lock().unwrap() = None;
        return Err(format!(
            "не удалось поставить хук мыши (код {}). Попробуйте запустить \
             gmouse от имени администратора",
            unsafe { GetLastError() }
        ));
    }

    unsafe {
        if test_mode {
            println!("Режим теста. Нажимайте кнопки мыши — печатаются их имена.");
        } else {
            println!("Активные привязки:");
            if state.bindings.is_empty() {
                println!("  (нет — кнопки работают как обычно)");
            }
            for (button, binding) in &state.bindings {
                let arg = binding
                    .arg
                    .as_deref()
                    .map(|a| format!(" ({a})"))
                    .unwrap_or_default();
                println!("  {:<11} -> {}{}", button.name(), binding.action, arg);
            }
        }
        println!("\nCtrl+Alt+Q — остановить. Окно можно свернуть, оно не нужно.");

        let mut msg = MSG::default();
        while state.running.load(Ordering::SeqCst) {
            let got = GetMessageW(&mut msg, 0, 0, 0);
            if got == 0 || got == -1 {
                break;
            }
        }

        UnhookWindowsHookEx(mouse_hook);
        if keyboard_hook != 0 {
            UnhookWindowsHookEx(keyboard_hook);
        }
    }
    *STATE.lock().unwrap() = None;
    println!("Остановлено.");
    Ok(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn button_names_roundtrip() {
        for b in [Button::X1, Button::X2, Button::Middle, Button::WheelUp] {
            assert_eq!(Button::parse(b.name()), Ok(b));
        }
    }

    #[test]
    fn aliases() {
        assert_eq!(Button::parse("back"), Ok(Button::X1));
        assert_eq!(Button::parse("Browser_Forward"), Ok(Button::X2));
        assert_eq!(Button::parse("wheel-click"), Ok(Button::Middle));
        assert!(Button::parse("нет-такой").is_err());
    }

    #[test]
    fn bindings_drop_unknown_buttons() {
        let list = vec![
            Binding {
                button: "x1".into(),
                action: "double_click".into(),
                arg: None,
            },
            Binding {
                button: "чепуха".into(),
                action: "none".into(),
                arg: None,
            },
        ];
        let (map, warnings) = build_bindings(&list);
        assert_eq!(map.len(), 1);
        assert_eq!(warnings.len(), 1);
    }
}
