//! Поиск vendor-протокола для аппаратного DPI.
//!
//! У мышей без официальной утилиты настройки (а у большинства китайских
//! контроллеров — именно так) DPI меняется «магической» командой в
//! feature/output-репорте служебного HID-интерфейса (usage page 0xFF00).
//! Формат у каждого контроллера свой, но наборов реально немного.
//!
//! Модуль умеет:
//!   * показать служебные интерфейсы и длину их репортов;
//!   * перебрать известные наборы команд и показать ответ устройства;
//!   * провести интерактивную идентификацию: отправить команду, спросить
//!     «изменилась ли скорость курсора?» — и записать рабочий набор в профиль.
//!
//! Так «магический» протокол определяется за минуту, без анализа прошивки.

use crate::hid;
use crate::win::HANDLE;
use std::io::{self, Write};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Transport {
    /// Feature-репорт: HidD_SetFeature, ответ — следующий HidD_GetFeature
    Feature,
    /// Output-репорт: HidD_SetOutputReport (без ответа)
    Output,
}

impl Transport {
    pub fn parse(text: &str) -> Option<Transport> {
        match text.to_lowercase().as_str() {
            "feature" => Some(Transport::Feature),
            "output" | "out" => Some(Transport::Output),
            _ => None,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Transport::Feature => "feature",
            Transport::Output => "output",
        }
    }
}

/// Один кандидат: как именно отправлять DPI.
#[derive(Debug, Clone)]
pub struct Candidate {
    pub name: &'static str,
    pub transport: Transport,
    pub command_id: u8,
    pub sub_id: u8,
    /// Порядок байт DPI: true — младший байт первым
    pub little_endian: bool,
    /// Смещение, с которого начинается DPI в репорте
    pub offset: usize,
    /// Размер репорта (9 — стандартный feature-репорт HID)
    pub length: usize,
}

impl Candidate {
    pub fn build(&self, dpi: u32) -> Vec<u8> {
        let mut buf = vec![0u8; self.length.max(self.offset + 2)];
        buf[0] = self.command_id;
        if self.length > 1 {
            buf[1] = self.sub_id;
        }
        let lo = (dpi & 0xFF) as u8;
        let hi = ((dpi >> 8) & 0xFF) as u8;
        if self.little_endian {
            buf[self.offset] = lo;
            buf[self.offset + 1] = hi;
        } else {
            buf[self.offset] = hi;
            buf[self.offset + 1] = lo;
        }
        buf
    }

    /// Собрать кандидата из сохранённого в профиле протокола.
    pub fn from_protocol(
        command_id: u8,
        sub_id: u8,
        offset: usize,
        transport: &str,
    ) -> Candidate {
        Candidate {
            name: "из профиля",
            transport: Transport::parse(transport).unwrap_or(Transport::Feature),
            command_id,
            sub_id,
            little_endian: true,
            offset,
            length: 9,
        }
    }

    pub fn describe(&self) -> String {
        format!(
            "{:<28} {} cmd=0x{:02X} sub=0x{:02X} offset={} {}",
            self.name,
            self.transport.name(),
            self.command_id,
            self.sub_id,
            self.offset,
            if self.little_endian { "LE" } else { "BE" }
        )
    }
}

/// Известные наборы команд, встречающиеся у бюджетных игровых мышей.
/// Порядок — от самых распространённых.
pub fn candidates() -> Vec<Candidate> {
    let mut out = Vec::new();
    // 1. «0x07 0x05» — очень частый вариант (SONiX/Compx и клоны)
    for &(name, transport) in &[
        ("7-byte 0x07/0x05", Transport::Feature),
        ("7-byte 0x07/0x05 out", Transport::Output),
    ] {
        out.push(Candidate {
            name,
            transport,
            command_id: 0x07,
            sub_id: 0x05,
            little_endian: true,
            offset: 2,
            length: 9,
        });
    }
    // 2. «0x07 0x04» (некоторые Sino Wealth)
    out.push(Candidate {
        name: "0x07/0x04 LE",
        transport: Transport::Feature,
        command_id: 0x07,
        sub_id: 0x04,
        little_endian: true,
        offset: 2,
        length: 9,
    });
    // 3. «0x08 0x07»
    out.push(Candidate {
        name: "0x08/0x07 LE",
        transport: Transport::Feature,
        command_id: 0x08,
        sub_id: 0x07,
        little_endian: true,
        offset: 3,
        length: 9,
    });
    // 4. «0x03 0x01» (часто у 2.4 ГГц «офисных»)
    out.push(Candidate {
        name: "0x03/0x01 LE",
        transport: Transport::Feature,
        command_id: 0x03,
        sub_id: 0x01,
        little_endian: true,
        offset: 2,
        length: 9,
    });
    // 5. «0x04 0x01» little/big
    out.push(Candidate {
        name: "0x04/0x01 LE",
        transport: Transport::Feature,
        command_id: 0x04,
        sub_id: 0x01,
        little_endian: true,
        offset: 2,
        length: 9,
    });
    out.push(Candidate {
        name: "0x04/0x01 BE",
        transport: Transport::Feature,
        command_id: 0x04,
        sub_id: 0x01,
        little_endian: false,
        offset: 2,
        length: 9,
    });
    // 6. 8-байтовые репорты (некоторые 2.4 ГГц)
    out.push(Candidate {
        name: "0x07/0x05 8-byte",
        transport: Transport::Feature,
        command_id: 0x07,
        sub_id: 0x05,
        little_endian: true,
        offset: 2,
        length: 8,
    });
    out
}

pub struct Target {
    pub handle: HANDLE,
}

impl Target {
    pub fn send(&self, candidate: &Candidate, dpi: u32) -> Option<Vec<u8>> {
        let payload = candidate.build(dpi);
        match candidate.transport {
            Transport::Feature => hid::feature_request(self.handle, &payload),
            Transport::Output => {
                if hid::output_report(self.handle, &payload) {
                    Some(payload)
                } else {
                    None
                }
            }
        }
    }
}

pub struct ProbeResult {
    pub candidate: Candidate,
    pub response: Option<Vec<u8>>,
}

pub fn hex(bytes: &[u8]) -> String {
    bytes
        .iter()
        .map(|b| format!("{b:02X}"))
        .collect::<Vec<_>>()
        .join(" ")
}

/// Перебрать всех кандидатов и вернуть те, на которые устройство ответило.
pub fn probe_all(target: &Target, dpi: u32) -> Vec<ProbeResult> {
    let mut results = Vec::new();
    for candidate in candidates() {
        let response = target.send(&candidate, dpi);
        results.push(ProbeResult {
            candidate,
            response,
        });
    }
    results
}

/// Интерактивная идентификация: пробуем кандидатов по одному,
/// после каждой попытки спрашиваем, изменилась ли скорость курсора.
/// Возвращает рабочего кандидата.
pub fn identify(target: &Target, low: u32, high: u32) -> Option<Candidate> {
    let mut asked = 0;
    for candidate in candidates() {
        asked += 1;
        println!("\n[{asked}] {}", candidate.describe());
        let before = target.send(&candidate, low);
        match &before {
            Some(reply) => println!("    ответ: {}", hex(reply)),
            None => println!("    ответа нет (feature/output не принят)"),
        }
        println!(
            "    Отправлен DPI = {low}. Курсор стал заметно медленнее? \
             [y] да  [n] нет  [s] пропустить  [q] выход"
        );
        print!("    > ");
        let _ = io::stdout().flush();
        let mut answer = String::new();
        if io::stdin().read_line(&mut answer).is_err() {
            return None;
        }
        match answer.trim().to_lowercase().as_str() {
            "y" | "yes" | "д" | "да" => {
                let _ = target.send(&candidate, high);
                println!(
                    "    DPI = {high} отправлен. Курсор вернулся к быстрому? [y/n]"
                );
                print!("    > ");
                let _ = io::stdout().flush();
                let mut confirm = String::new();
                let _ = io::stdin().read_line(&mut confirm);
                let ok = matches!(
                    confirm.trim().to_lowercase().as_str(),
                    "y" | "yes" | "д" | "да"
                );
                if ok {
                    println!("\nНайден рабочий набор: {}", candidate.describe());
                    return Some(candidate);
                }
                println!("    Не подтвердилось, пробуем следующий.");
                let _ = target.send(&candidate, 800);
            }
            "q" | "quit" | "й" => return None,
            _ => {
                let _ = target.send(&candidate, 800);
            }
        }
    }
    println!("\nНи один кандидат не подошёл.");
    println!(
        "Соберите `gmouse diag` и пришлите отчёт: по дампу репортов \
         можно определить формат протокола вашего контроллера."
    );
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn builds_little_endian_dpi() {
        let candidate = Candidate {
            name: "t",
            transport: Transport::Feature,
            command_id: 0x07,
            sub_id: 0x05,
            little_endian: true,
            offset: 2,
            length: 9,
        };
        let buf = candidate.build(1600);
        assert_eq!(buf.len(), 9);
        assert_eq!(buf[0], 0x07);
        assert_eq!(buf[1], 0x05);
        assert_eq!(buf[2], 0x40); // 1600 = 0x0640
        assert_eq!(buf[3], 0x06);
    }

    #[test]
    fn builds_big_endian_dpi() {
        let candidate = Candidate {
            name: "t",
            transport: Transport::Feature,
            command_id: 0x04,
            sub_id: 0x01,
            little_endian: false,
            offset: 2,
            length: 9,
        };
        let buf = candidate.build(800);
        assert_eq!(&buf[2..4], &[0x03, 0x20]);
    }

    #[test]
    fn all_candidates_have_distinct_descriptions() {
        let list = candidates();
        assert!(list.len() >= 6);
        let mut names: Vec<_> = list.iter().map(|c| c.describe()).collect();
        names.sort();
        names.dedup();
        assert_eq!(names.len(), list.len());
    }
}
