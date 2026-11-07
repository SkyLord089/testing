//! Действия, которые можно повесить на кнопку мыши.
//!
//! Действие — это строка из конфига, например:
//!   "left" / "right" / "middle" / "x1" / "x2"   — клик
//!   "double_click", "triple_click"
//!   "wheel"  с arg = ±N                         — прокрутка
//!   "hscroll" с arg = ±N                        — горизонтальная прокрутка
//!   "hotkey" с arg = "ctrl+shift+esc"           — сочетание клавиш
//!   "text"   с arg = "привет"                   — «печать» текста
//!   "app"    с arg = "notepad.exe"              — запуск программы
//!   "url"    с arg = "https://..."              — открыть ссылку
//!   "none"                                      — проглотить нажатие
//!
//! Синтез ввода идёт через `SendInput` с меткой G_MOUSE_MARK, поэтому
//! хук не перехватывает собственные события и не зацикливается.

use crate::win::*;
use std::os::windows::ffi::OsStrExt;
use std::process::Command;

pub const WHEEL_DELTA: i32 = 120;

fn send(inputs: &[INPUT]) {
    unsafe {
        SendInput(inputs.len() as u32, inputs.as_ptr(), std::mem::size_of::<INPUT>() as i32);
    }
}

fn click(down: u32, up: u32, data: i32) {
    send(&[INPUT::mouse(down, data), INPUT::mouse(up, data)]);
}

/// Сколько нажатий и с какой паузой делает «многократный клик».
const MULTI_CLICK_GAP_MS: u64 = 40;

pub fn click_button(name: &str) -> bool {
    match name {
        "left" => {
            click(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 0);
            true
        }
        "right" => {
            click(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, 0);
            true
        }
        "middle" => {
            click(MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, 0);
            true
        }
        "x1" => {
            click(MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, 1);
            true
        }
        "x2" => {
            click(MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, 2);
            true
        }
        _ => false,
    }
}

pub fn wheel(notches: i32) {
    send(&[INPUT::mouse(MOUSEEVENTF_WHEEL, notches * WHEEL_DELTA)]);
}

pub fn hwheel(chars: i32) {
    send(&[INPUT::mouse(MOUSEEVENTF_HWHEEL, chars * WHEEL_DELTA)]);
}

/// Виртуальные коды клавиш, которые встречаются в сокращениях.
fn vk_for(name: &str) -> Option<u16> {
    let name = name.trim().to_lowercase();
    let vk = match name.as_str() {
        "ctrl" | "control" => 0x11,
        "shift" => 0x10,
        "alt" => 0x12,
        "win" | "lwin" => 0x5B,
        "esc" | "escape" => 0x1B,
        "enter" | "return" => 0x0D,
        "tab" => 0x09,
        "space" => 0x20,
        "backspace" => 0x08,
        "delete" | "del" => 0x2E,
        "insert" => 0x2D,
        "home" => 0x24,
        "end" => 0x23,
        "pageup" | "pgup" => 0x21,
        "pagedown" | "pgdn" => 0x22,
        "left" => 0x25,
        "right" => 0x27,
        "up" => 0x26,
        "down" => 0x28,
        "capslock" => 0x14,
        "printscreen" => 0x2C,
        "pause" => 0x13,
        "f1" => 0x70,
        "f2" => 0x71,
        "f3" => 0x72,
        "f4" => 0x73,
        "f5" => 0x74,
        "f6" => 0x75,
        "f7" => 0x76,
        "f8" => 0x77,
        "f9" => 0x78,
        "f10" => 0x79,
        "f11" => 0x7A,
        "f12" => 0x7B,
        "volumeup" | "volup" => 0xAF,
        "volumedown" | "voldown" => 0xAE,
        "volumemute" | "mute" => 0xAD,
        "medianext" | "nexttrack" => 0xB0,
        "mediaprev" | "prevtrack" => 0xB1,
        "mediastop" => 0xB2,
        "mediaplay" | "playpause" => 0xB3,
        _ => {
            let mut chars = name.chars();
            let first = chars.next()?;
            if chars.next().is_none() && first.is_ascii_alphanumeric() {
                // "a".."z" -> 0x41.., "0".."9" -> 0x30..
                return Some(first.to_ascii_uppercase() as u16);
            }
            return None;
        }
    };
    Some(vk)
}

/// Нажать сочетание вида "ctrl+shift+esc".
pub fn hotkey(combination: &str) -> bool {
    let parts: Vec<u16> = combination
        .split('+')
        .filter_map(|part| vk_for(part))
        .collect();
    if parts.is_empty() {
        return false;
    }
    let mut inputs = Vec::with_capacity(parts.len() * 2);
    for vk in &parts {
        inputs.push(INPUT::key(*vk, false));
    }
    for vk in parts.iter().rev() {
        inputs.push(INPUT::key(*vk, true));
    }
    send(&inputs);
    true
}

/// «Напечатать» строку через Unicode-ввод (KEYEVENTF_UNICODE: wVk = 0,
/// wScan = кодовая единица UTF-16) — работает для любого языка.
pub fn type_text(text: &str) -> bool {
    const KEYEVENTF_UNICODE: u32 = 0x0004;
    // VkKeyScanW/MapVirtualKeyW не нужны: Unicode-ввод работает без них.
    let mut inputs = Vec::new();
    for unit in text.encode_utf16() {
        inputs.push(INPUT {
            type_: INPUT_KEYBOARD,
            u: INPUT_UNION {
                ki: KEYBDINPUT {
                    wVk: 0,
                    wScan: unit,
                    dwFlags: KEYEVENTF_UNICODE,
                    time: 0,
                    dwExtraInfo: G_MOUSE_MARK,
                },
            },
        });
        inputs.push(INPUT {
            type_: INPUT_KEYBOARD,
            u: INPUT_UNION {
                ki: KEYBDINPUT {
                    wVk: 0,
                    wScan: unit,
                    dwFlags: KEYEVENTF_UNICODE | KEYEVENTF_KEYUP,
                    time: 0,
                    dwExtraInfo: G_MOUSE_MARK,
                },
            },
        });
    }
    if inputs.is_empty() {
        return false;
    }
    send(&inputs);
    true
}

pub fn launch(command: &str) -> bool {
    let mut parts = command.split_whitespace();
    let Some(exe) = parts.next() else {
        return false;
    };
    Command::new(exe).args(parts).spawn().is_ok()
}

pub fn open_url(url: &str) -> bool {
    // ShellExecuteW через shell32
    #[link(name = "shell32")]
    extern "system" {
        fn ShellExecuteW(hwnd: HWND, op: LPCWSTR, file: LPCWSTR, params: LPCWSTR,
                         dir: LPCWSTR, show: i32) -> HANDLE;
    }
    let to_wide = |s: &str| -> Vec<u16> {
        std::ffi::OsStr::new(s)
            .encode_wide()
            .chain(std::iter::once(0))
            .collect()
    };
    let op = to_wide("open");
    let file = to_wide(url);
    unsafe {
        let result = ShellExecuteW(0, op.as_ptr(), file.as_ptr(),
                                   std::ptr::null(), std::ptr::null(), 1);
        result as usize > 32
    }
}

/// Несколько кликов подряд с короткой паузой: без паузы часть игр и
/// приложений склеивает их в один клик.
pub fn multi_click(times: u32) -> bool {
    for i in 0..times {
        if i > 0 {
            std::thread::sleep(std::time::Duration::from_millis(MULTI_CLICK_GAP_MS));
        }
        click_button("left");
    }
    true
}

/// Выполнить действие по имени. Возвращает true, если нажатие надо съесть.
pub fn perform(action: &str, arg: Option<&str>) -> bool {
    match action {
        "none" => true,
        "left" | "right" | "middle" | "x1" | "x2" => click_button(action),
        "double_click" => multi_click(2),
        "triple_click" => multi_click(3),
        "wheel" => {
            let notches = arg.and_then(|a| a.parse::<i32>().ok()).unwrap_or(3);
            wheel(notches);
            true
        }
        "hscroll" => {
            let chars = arg.and_then(|a| a.parse::<i32>().ok()).unwrap_or(3);
            hwheel(chars);
            true
        }
        "hotkey" => hotkey(arg.unwrap_or("")),
        "text" => type_text(arg.unwrap_or("")),
        "app" => launch(arg.unwrap_or("")),
        "url" => open_url(arg.unwrap_or("")),
        _ => false,
    }
}
