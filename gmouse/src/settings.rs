//! Системные настройки мыши, которые доступны без vendor-драйвера:
//! скорость курсора (1..20, как в «Параметрах Windows»), строки прокрутки,
//! горизонтальная прокрутка, порог «двойного клика» и залипания кнопок.
//!
//! Это не аппаратный DPI сенсора (его меняет только прошивка мыши),
//! но именно эти значения Windows применяет к каждому движению.

use crate::win::*;
use std::ffi::c_void;

fn spi_get_u32(action: u32) -> Option<u32> {
    let mut value: u32 = 0;
    let ok = unsafe {
        SystemParametersInfoW(action, 0, &mut value as *mut u32 as *mut c_void, 0)
    };
    if ok != 0 {
        Some(value)
    } else {
        None
    }
}

/// SPI_SETMOUSESPEED / SPI_SETWHEELSCROLLLINES / SPI_SETWHEELSCROLLCHARS
/// принимают значение в pvParam как целое (приведённое к указателю), а не
/// адрес переменной — так это реализовано в Windows и Wine.
fn spi_set_value(action: u32, value: u32) -> bool {
    let ok = unsafe {
        SystemParametersInfoW(
            action,
            0,
            value as usize as *mut c_void,
            SPIF_UPDATEINIFILE | SPIF_SENDCHANGE,
        )
    };
    ok != 0
}

#[derive(Debug, Clone, PartialEq)]
pub struct SystemMouse {
    /// 1..20 — «скорость» курсора в параметрах Windows
    pub cursor_speed: u32,
    /// Сколько строк прокручивать за одно деление колеса
    pub scroll_lines: u32,
    /// Сколько символов прокручивать по горизонтали
    pub hscroll_chars: u32,
    pub buttons_swapped: bool,
    pub double_click_width: i32,
    pub double_click_height: i32,
}

impl SystemMouse {
    pub fn read() -> SystemMouse {
        let mut thresholds = [0i32; 3];
        unsafe {
            SystemParametersInfoW(
                SPI_GETMOUSE,
                0,
                thresholds.as_mut_ptr() as *mut c_void,
                0,
            );
        }
        SystemMouse {
            cursor_speed: spi_get_u32(SPI_GETMOUSESPEED).unwrap_or(10),
            scroll_lines: spi_get_u32(SPI_GETWHEELSCROLLLINES).unwrap_or(3),
            hscroll_chars: spi_get_u32(SPI_GETWHEELSCROLLCHARS).unwrap_or(3),
            buttons_swapped: unsafe { GetSystemMetrics(SM_SWAPBUTTON) } != 0,
            double_click_width: unsafe { GetSystemMetrics(SM_CXDOUBLECLK) },
            double_click_height: unsafe { GetSystemMetrics(SM_CYDOUBLECLK) },
        }
    }

    pub fn set_cursor_speed(&self, speed: u32) -> Result<(), String> {
        if !(1..=20).contains(&speed) {
            return Err("скорость курсора должна быть от 1 до 20".into());
        }
        if spi_set_value(SPI_SETMOUSESPEED, speed) {
            Ok(())
        } else {
            Err("Windows не приняла новую скорость (нужны права?)".into())
        }
    }

    pub fn set_scroll_lines(&self, lines: u32) -> Result<(), String> {
        // 0xFFFFFFFF = «на страницу целиком», как в настройках Windows
        if lines != u32::MAX && !(1..=100).contains(&lines) {
            return Err("строк на деление колеса: от 1 до 100 (или 4294967295 = страница)".into());
        }
        if spi_set_value(SPI_SETWHEELSCROLLLINES, lines) {
            Ok(())
        } else {
            Err("не удалось изменить прокрутку колеса".into())
        }
    }

    pub fn set_hscroll_chars(&self, chars: u32) -> Result<(), String> {
        if !(1..=100).contains(&chars) {
            return Err("символов горизонтальной прокрутки: от 1 до 100".into());
        }
        if spi_set_value(SPI_SETWHEELSCROLLCHARS, chars) {
            Ok(())
        } else {
            Err("не удалось изменить горизонтальную прокрутку".into())
        }
    }

    /// Ускорение указателя: порог1, порог2, ускорение (0 — выключено).
    pub fn set_acceleration(&self, enabled: bool) -> Result<(), String> {
        let values: [i32; 3] = if enabled { [6, 10, 1] } else { [0, 0, 0] };
        let ok = unsafe {
            SystemParametersInfoW(
                SPI_SETMOUSE,
                0,
                values.as_ptr() as *mut c_void,
                SPIF_UPDATEINIFILE | SPIF_SENDCHANGE,
            )
        };
        if ok != 0 {
            Ok(())
        } else {
            Err("не удалось изменить ускорение указателя".into())
        }
    }

    /// Соответствие «скорости Windows» (1..20) множителю перемещения.
    pub fn speed_multiplier(&self) -> f64 {
        // Таблица из документации Microsoft: 1 -> 1/32 ... 10 -> 1 ... 20 -> 3.5
        const TABLE: [f64; 21] = [
            0.0, 0.03125, 0.0625, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875,
            1.0, 1.25, 1.5, 1.75, 2.0, 2.25, 2.5, 2.75, 3.0, 3.25, 3.5,
        ];
        let idx = self.cursor_speed.clamp(1, 20) as usize;
        TABLE[idx]
    }

    /// Эффективные «точки на дюйм» с учётом скорости Windows.
    /// Не заменяет аппаратный DPI, но показывает итог, который видит курсор.
    pub fn effective_cpi(&self, sensor_dpi: u32) -> f64 {
        sensor_dpi as f64 * self.speed_multiplier()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample(speed: u32) -> SystemMouse {
        SystemMouse {
            cursor_speed: speed,
            scroll_lines: 3,
            hscroll_chars: 3,
            buttons_swapped: false,
            double_click_width: 4,
            double_click_height: 4,
        }
    }

    #[test]
    fn speed_table_matches_windows() {
        assert!((sample(10).speed_multiplier() - 1.0).abs() < 1e-9);
        assert!((sample(1).speed_multiplier() - 0.03125).abs() < 1e-9);
        assert!((sample(20).speed_multiplier() - 3.5).abs() < 1e-9);
    }

    #[test]
    fn effective_cpi_scales() {
        assert!((sample(20).effective_cpi(800) - 2800.0).abs() < 1e-6);
    }

    #[test]
    fn out_of_range_speed_rejected() {
        assert!(sample(10).set_cursor_speed(0).is_err());
        assert!(sample(10).set_cursor_speed(21).is_err());
    }
}
