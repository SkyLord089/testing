//! Протокол Attack Shark (Beken BK3630/BK3631 + PixArt PAW3395/PAW3311).
//!
//! У этих мышей **два разных транспорта**, и это главное, что нужно знать:
//!
//!   * **кабель** (`1d57:2126`, `1d57:2120` и др.) — настройки ходят
//!     feature-репортами: `SET_FEATURE(0xa0 ...)` просит блок, `GET_FEATURE`
//!     читает; запись — `SET_FEATURE(блок)`. Никаких внешних кадров.
//!   * **приёмник 2.4 ГГц** (`1d57:fa65`, `fa60`, `fa61`, `fa55`) — настройки
//!     ходят input/output-репортами с Report ID 4: внешний кадр
//!     `04 len opcode … chkHi chkLo`, а приёмник отвечает статусами
//!     `0xc0`/`0xc1`/`0xc2`/`0xc3`. Радиоканал теряет ~5 % запросов,
//!     поэтому чтение обязательно с повторами.
//!
//! Внутри обоих транспортов — одинаковый «блок»:
//! `[cmd, size, profile, поля…, контрольная сумма (2), нули]`.
//!
//! Источники формата: публичная документация OpenMouse / attackshark-bridge /
//! LinShark (X8 Pro, X8 SE, X11). Всё, что не подтверждено, помечено в коде.

use crate::hid::{self, HidInfo};
use crate::win::HANDLE;

// ------------------------------------------------------------------ константы
pub const CMD_DPI: u8 = 0x04;
pub const CMD_LIGHTING: u8 = 0x05;
pub const CMD_POLLING: u8 = 0x06;
pub const CMD_BUTTONS: u8 = 0x08;
pub const CMD_PROFILE: u8 = 0x0A;
pub const CMD_FIRMWARE: u8 = 0x0B;
pub const CMD_READ: u8 = 0xA0;

/// Размер блока на приёмнике (включая cmd и size).
pub const RECEIVER_SIZE: u16 = 56;
/// Объявленный размер feature-репорта на кабеле (включая Report ID).
pub const CABLE_SIZE: u16 = 52;
pub const POLLING_SIZE: u16 = 9;
pub const FIRMWARE_SIZE: u16 = 8;

const OFF_STAGE_MASK_A: usize = 6;
const OFF_STAGE_MASK_B: usize = 7;
const OFF_STAGES: usize = 8;
const OFF_HIGH: usize = 16;
const OFF_ACTIVE_STAGE: usize = 24;
const OFF_CHECKSUM: usize = 50;

/// Сколько раз повторять запрос на приёмнике: радиоканал теряет пакеты.
const RECEIVER_RETRIES: usize = 6;

// ------------------------------------------------------------------ кодировки
/// Как модель хранит DPI.
///
/// * `Linear` — `dpi = (n + 1) * 50` (X8 Pro; подтверждено измерением счётчиков
///   сенсора линейкой).
/// * `StepTable` — индекс в таблице ступеней PAW3311 (X8 SE, X11).
///
/// Это единственное место, где модели расходятся, поэтому кодировку можно
/// переключить вручную: `gmouse shark encoding table|linear`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Encoding {
    Linear,
    StepTable,
}

impl Encoding {
    pub fn parse(text: &str) -> Option<Encoding> {
        match text.to_lowercase().as_str() {
            "linear" | "линейно" => Some(Encoding::Linear),
            "table" | "step" | "pa3311" | "таблица" => Some(Encoding::StepTable),
            _ => None,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Encoding::Linear => "linear",
            Encoding::StepTable => "table",
        }
    }

    /// Кодировка по умолчанию для PID: X8 Pro (кабель/приёмник) — линейная,
    /// X8 SE (0x2120) и X11-семейство — таблица, для остальных считаем
    /// линейной (новые ревизии) до калибровки.
    pub fn for_pid(pid: u16) -> Encoding {
        match pid {
            0x2120 | 0xFA55 | 0xFA60 | 0xFA61 => Encoding::StepTable,
            _ => Encoding::Linear,
        }
    }
}

/// Таблица ступеней PAW3311: DPI = (индекс + 1) * 50 при yByte = 0
/// и (индекс + 1) * 100 при yByte = 1.
const DPI_3311: [u8; 200] = [
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1b, 0x1c, 0x1d, 0x1e,
    0x1f, 0x20, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x29, 0x2a, 0x2b, 0x2c, 0x2d,
    0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3d,
    0x3e, 0x3f, 0x40, 0x41, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x4a, 0x4b, 0x4c,
    0x4d, 0x4e, 0x4f, 0x51, 0x52, 0x53, 0x54, 0x55, 0x57, 0x58, 0x59, 0x5a, 0x5b,
    0x5c, 0x5e, 0x5f, 0x60, 0x61, 0x62, 0x63, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6b,
    0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x79, 0x7a,
    0x7b, 0x7c, 0x7d, 0x7f, 0x80, 0x81, 0x82, 0x83, 0x84, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x8b, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
    0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa7, 0xa8,
    0xa9, 0xaa, 0xab, 0xac, 0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xc0, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6,
    0xc7, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xcf, 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd6,
    0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdd, 0xde, 0xdf, 0xe0, 0xe1, 0xe3, 0xe4, 0xe5,
    0xe6, 0xe7, 0xe8, 0xea, 0xeb,
];

pub const MIN_DPI: u32 = 50;
pub const MAX_DPI: u32 = 20_000;

/// DPI -> (xByte, yByte, double) для выбранной кодировки.
pub fn encode_dpi(dpi: u32, encoding: Encoding) -> (u8, u8, bool) {
    let mut dpi = dpi.clamp(MIN_DPI, MAX_DPI);
    let double = dpi > 10_000;
    if double {
        // выше 10000 сенсор хранит половину значения
        dpi = (dpi as f64 / 2.0).round() as u32;
    }
    match encoding {
        Encoding::Linear => {
            let steps = (dpi / 50).max(1);
            (((steps - 1).min(255)) as u8, 0, double)
        }
        Encoding::StepTable => {
            if dpi > 5_000 && dpi % 100 == 0 {
                let index = (dpi / 100 - 1) as usize;
                return (DPI_3311[index.min(DPI_3311.len() - 1)], 1, double);
            }
            let index = (dpi.saturating_sub(50) / 50) as usize;
            (DPI_3311[index.min(DPI_3311.len() - 1)], 0, double)
        }
    }
}

/// (xByte, yByte, double) -> DPI. 0 означает «ступень выключена».
pub fn decode_dpi(x_byte: u8, y_byte: u8, double: bool, encoding: Encoding) -> u32 {
    if x_byte == 0 && y_byte == 0 {
        return 0;
    }
    let mut dpi = match encoding {
        Encoding::Linear => (x_byte as u32 + 1) * 50,
        Encoding::StepTable => match DPI_3311.iter().position(|b| *b == x_byte) {
            Some(index) if y_byte == 0 => index as u32 * 50 + 50,
            Some(index) if y_byte == 1 => index as u32 * 100 + 100,
            Some(_) => 10_100 + (((y_byte as u32) << 8 | x_byte as u32).saturating_sub(199)) * 100,
            None => x_byte as u32 * 50,
        },
    };
    if double {
        dpi = dpi.saturating_mul(2);
    }
    dpi
}

/// Контрольная сумма блока: сумма байт 3..49, big-endian в 50..51.
pub fn checksum(data: &[u8]) -> u16 {
    let end = OFF_CHECKSUM.min(data.len());
    let start = 3.min(end);
    let sum: u32 = data[start..end].iter().map(|b| *b as u32).sum();
    (sum & 0xFFFF) as u16
}

pub fn apply_checksum(data: &mut [u8]) -> bool {
    if data.len() < OFF_CHECKSUM + 2 {
        return false;
    }
    let sum = checksum(data);
    data[OFF_CHECKSUM] = (sum >> 8) as u8;
    data[OFF_CHECKSUM + 1] = (sum & 0xFF) as u8;
    true
}

// ------------------------------------------------------------------ профиль
#[derive(Debug, Clone, PartialEq)]
pub struct Profile {
    pub stages: Vec<u32>,
    pub active_stage: u8,
    pub polling_hz: Option<u16>,
    pub raw: Vec<u8>,
    pub encoding: Encoding,
}

pub fn parse_profile(raw: &[u8], encoding: Encoding) -> Option<Profile> {
    if raw.len() < OFF_ACTIVE_STAGE + 1 {
        return None;
    }
    let mut stages = Vec::with_capacity(8);
    let mask = *raw.get(OFF_STAGE_MASK_A).unwrap_or(&0);
    for i in 0..8 {
        let Some(x_byte) = raw.get(OFF_STAGES + i).copied() else {
            break;
        };
        let y_byte = raw.get(OFF_HIGH + i).copied().unwrap_or(0);
        let double = (mask >> i) & 1 == 1;
        stages.push(decode_dpi(x_byte, y_byte, double, encoding));
    }
    Some(Profile {
        stages,
        active_stage: raw[OFF_ACTIVE_STAGE].clamp(1, 8),
        polling_hz: None,
        raw: raw.to_vec(),
        encoding,
    })
}

pub fn set_active_dpi(profile: &mut Profile, dpi: u32) -> bool {
    if profile.raw.len() < OFF_CHECKSUM + 2 {
        return false;
    }
    let index = (profile.active_stage.saturating_sub(1) as usize).min(7);
    let (x_byte, y_byte, double) = encode_dpi(dpi, profile.encoding);
    profile.raw[OFF_STAGES + index] = x_byte;
    if profile.raw.len() > OFF_HIGH + index {
        profile.raw[OFF_HIGH + index] = y_byte;
    }
    // бит «x2»: документация отмечает, что клиенты иногда ставят его в байт 6,
    // иногда в 7 — ставим в оба, как делают официальные утилиты
    for mask_offset in [OFF_STAGE_MASK_A, OFF_STAGE_MASK_B] {
        if profile.raw.len() > mask_offset {
            if double {
                profile.raw[mask_offset] |= 1 << index;
            } else {
                profile.raw[mask_offset] &= !(1 << index);
            }
        }
    }
    profile.stages[index] = decode_dpi(x_byte, y_byte, double, profile.encoding);
    apply_checksum(&mut profile.raw)
}

pub fn set_active_stage(profile: &mut Profile, stage: u8) -> bool {
    if !(1..=8).contains(&stage) || profile.raw.len() <= OFF_ACTIVE_STAGE {
        return false;
    }
    profile.raw[OFF_ACTIVE_STAGE] = stage;
    profile.active_stage = stage;
    apply_checksum(&mut profile.raw)
}

// ------------------------------------------------------------------ опрос
pub fn decode_polling(byte: u8) -> Option<u16> {
    match byte {
        0x01 => Some(1000),
        0x02 => Some(500),
        0x04 => Some(250),
        0x08 => Some(125),
        _ => None,
    }
}

pub fn encode_polling(hz: u16) -> Option<u8> {
    Some(match hz {
        125 => 0x08,
        250 => 0x04,
        500 => 0x02,
        1000 => 0x01,
        _ => return None,
    })
}

// ------------------------------------------------------------------ батарея
/// Событие батареи: `03 <model> 40|41 <state> <percent>`.
pub fn parse_battery(event: &[u8]) -> Option<(u8, &'static str)> {
    if event.len() < 5 || event[0] != 0x03 || !matches!(event[2], 0x40 | 0x41) {
        return None;
    }
    let percent = event[4].min(100);
    let state = match event[3] {
        0x01 => "от батареи",
        0x02 => "полностью заряжено",
        0x03 => "заряжается",
        _ => "неизвестно",
    };
    Some((percent, state))
}

/// Событие смены DPI кнопкой: `03 <model> 10 <stage> 00`.
pub fn parse_dpi_event(event: &[u8]) -> Option<u8> {
    if event.len() < 5 || event[0] != 0x03 || event[2] != 0x10 {
        return None;
    }
    Some(event[3].clamp(1, 8))
}

// ------------------------------------------------------------------ транспорт
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Transport {
    /// Кабель: feature-репорты, Report ID = команда.
    Cable,
    /// Приёмник 2.4 ГГц: input/output Report ID 4 с внешним кадром.
    Receiver,
}

impl Transport {
    pub fn name(self) -> &'static str {
        match self {
            Transport::Cable => "кабель (feature-репорты)",
            Transport::Receiver => "приёмник 2.4 ГГц (report ID 4)",
        }
    }
}

/// Кадр запроса на чтение для приёмника (12 байт, вместе с Report ID).
pub fn receiver_read_request(cmd: u8, size: u16, profile: u8) -> Vec<u8> {
    let mut frame = vec![
        0x04,
        0x0C,
        0xA0,
        cmd,
        size as u8,
        profile,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
    ];
    let sum: u32 = frame[..10].iter().map(|b| *b as u32).sum();
    let sum = (sum & 0xFFFF) as u16;
    frame[10] = (sum >> 8) as u8;
    frame[11] = (sum & 0xFF) as u8;
    frame
}

/// Кадр записи для приёмника: `04 (size+5) 00 <блок> chkHi chkLo`.
pub fn receiver_write_frame(block: &[u8]) -> Vec<u8> {
    let mut frame = Vec::with_capacity(block.len() + 5);
    frame.push(0x04);
    frame.push((block.len() + 5) as u8);
    frame.push(0x00);
    frame.extend_from_slice(block);
    let sum: u32 = frame.iter().map(|b| *b as u32).sum();
    let sum = (sum & 0xFFFF) as u16;
    frame.push((sum >> 8) as u8);
    frame.push((sum & 0xFF) as u8);
    frame
}

pub struct Device {
    pub handle: HANDLE,
    pub read_handle: Option<HANDLE>,
    pub info: HidInfo,
    pub transport: Transport,
    pub encoding: Encoding,
    pub profile_index: u8,
}

impl Device {
    /// Блок целиком: чтение с повторами и проверкой контрольной суммы.
    pub fn read_block(&self, cmd: u8, size: u16) -> Result<Vec<u8>, String> {
        match self.transport {
            Transport::Cable => self.read_block_cable(cmd, size),
            Transport::Receiver => self.read_block_receiver(cmd, size),
        }
    }

    /// Запрос на чтение для feature-транспорта.
    ///
    /// Официальная утилита: `a0 cmd size 00 profile 00 00 00`.
    /// У X11-семейства (1d57:fa55/fa60/fa61) байт profile бывает важен:
    /// `0` в нём == «запрос проигнорирован», поэтому пробуем 1, потом 0.
    fn read_request(cmd: u8, size: u16, profile: u8) -> [u8; 8] {
        [CMD_READ, cmd, size as u8, 0x00, profile, 0x00, 0x00, 0x00]
    }

    fn read_block_cable(&self, cmd: u8, size: u16) -> Result<Vec<u8>, String> {
        // Размер чтения 64 байта: столько отдаёт Windows, и Profile (128)
        // приходит этим же каналом (первый кусок).
        let length = size.max(64);
        let profiles = [
            self.profile_index,
            self.profile_index.wrapping_add(1),
            0x00,
        ];
        let mut errors: Vec<String> = Vec::new();

        for profile_byte in profiles {
            let request = Self::read_request(cmd, size, profile_byte);
            if hid::write_feature(self.handle, &request).is_none() {
                errors.push(format!(
                    "SET_FEATURE(0xa0) отклонён (profile={profile_byte})"
                ));
                // STALL бывает разовым — повторяем, как официальная утилита
                std::thread::sleep(std::time::Duration::from_millis(210));
                if hid::write_feature(self.handle, &request).is_none() {
                    continue;
                }
            }

            // 1) ждём готовности: a0 01 ...
            let mut ready = false;
            for _ in 0..12 {
                std::thread::sleep(std::time::Duration::from_millis(60));
                if let Some(reply) = hid::read_feature_id(self.handle, CMD_READ, 8) {
                    let status = reply.get(1).copied().unwrap_or(0);
                    if status == 1 {
                        ready = true;
                        break;
                    }
                    // 0 = ещё не готово; прочие значения — устройство ответило
                    // другим кодом, но данные может отдать всё равно
                }
            }
            if !ready {
                errors.push(format!(
                    "нет флага готовности 0xA0 (profile={profile_byte})"
                ));
            }
            std::thread::sleep(std::time::Duration::from_millis(110));

            // 2) читаем сам блок
            let Some(block) = hid::read_feature_id(self.handle, cmd, length) else {
                errors.push(format!("GET_FEATURE(0x{cmd:02X}) не вернул данные"));
                continue;
            };
            match block.first().copied() {
                Some(id) if id == cmd => return Ok(block),
                Some(other) => errors.push(format!(
                    "в ответе чужая команда 0x{other:02X} вместо 0x{cmd:02X}"
                )),
                None => errors.push("пустой ответ".into()),
            }
        }

        Err(format!(
            "не удалось прочитать блок 0x{cmd:02X}: {}",
            errors.join("; ")
        ))
    }

    fn read_block_receiver(&self, cmd: u8, size: u16) -> Result<Vec<u8>, String> {
        let Some(read_handle) = self.read_handle else {
            return Err("нет канала чтения от приёмника".into());
        };
        let mut last_error = String::from("нет ответа");
        for attempt in 0..RECEIVER_RETRIES {
            let frame = receiver_read_request(cmd, size, self.profile_index);
            if hid::write_output_report(self.handle, &frame).is_none() {
                last_error = "не удалось отправить запрос в приёмник".into();
                continue;
            }
            match self.await_response(read_handle, cmd) {
                Ok(block) if block.len() >= 2 => return Ok(block),
                Ok(block) => last_error = format!("короткий ответ: {} байт", block.len()),
                Err(message) => last_error = message,
            }
            // приёмник не любит частые повторы
            std::thread::sleep(std::time::Duration::from_millis(180 + 100 * attempt as u64));
        }
        Err(format!(
            "приёмник не отдал блок 0x{cmd:02X} после {RECEIVER_RETRIES} попыток: {last_error}\n\
             Подсказка: на 2.4 ГГц дальние чтения часто не проходят. \
             Подключите мышь кабелем — по проводу настройки работают стабильно."
        ))
    }

    fn await_response(&self, read_handle: HANDLE, cmd: u8) -> Result<Vec<u8>, String> {
        let deadline = std::time::Instant::now() + std::time::Duration::from_millis(2500);
        while std::time::Instant::now() < deadline {
            let Some(report) = hid::read_input_timeout(read_handle, 64, 300) else {
                continue;
            };
            if report.len() < 4 {
                continue;
            }
            match report[2] {
                0xC0 | 0xC1 => {}
                0xC2 => return Err("приёмник: мышь не ответила (0xC2)".into()),
                0xC3 => return Err("приёмник занят (0xC3)".into()),
                0x00 => {
                    if report[3] == cmd {
                        return Ok(report[3..].to_vec());
                    }
                }
                _ => {}
            }
        }
        Err("таймаут ожидания ответа".into())
    }

    pub fn write_block(&self, block: &[u8]) -> Result<(), String> {
        match self.transport {
            Transport::Cable => {
                // SET_FEATURE может ответить STALL — повторяем, как делает
                // официальная утилита (через ~200 мс).
                let mut last = String::from("запись отклонена");
                for _ in 0..3 {
                    if hid::write_feature(self.handle, block).is_some() {
                        // даём мыши применить блок: без паузы чтение вернёт старое
                        std::thread::sleep(std::time::Duration::from_millis(500));
                        return Ok(());
                    }
                    last = "SET_FEATURE отклонён (STALL)".into();
                    std::thread::sleep(std::time::Duration::from_millis(210));
                }
                Err(last)
            }
            Transport::Receiver => {
                let frame = receiver_write_frame(block);
                hid::write_output_report(self.handle, &frame)
                    .ok_or_else(|| "приёмник не принял кадр записи".to_string())?;
                std::thread::sleep(std::time::Duration::from_millis(600));
                Ok(())
            }
        }
    }

    pub fn read_profile(&self) -> Result<Profile, String> {
        let size = match self.transport {
            Transport::Cable => CABLE_SIZE,
            Transport::Receiver => RECEIVER_SIZE,
        };
        let raw = self.read_block(CMD_DPI, size)?;
        let mut profile = parse_profile(&raw, self.encoding)
            .ok_or("блок DPI короче ожидаемого (нет байта активной ступени)")?;
        if let Ok(polling) = self.read_polling() {
            profile.polling_hz = Some(polling);
        }
        Ok(profile)
    }

    pub fn read_polling(&self) -> Result<u16, String> {
        let raw = self.read_block(CMD_POLLING, POLLING_SIZE)?;
        let byte = *raw.get(1).ok_or("пустой блок частоты опроса")?;
        let rate = decode_polling(byte).or_else(|| decode_polling(byte & 0x0F));
        rate.ok_or_else(|| format!("неизвестный код частоты 0x{byte:02X}"))
    }

    pub fn write_polling(&self, hz: u16) -> Result<u16, String> {
        let code = encode_polling(hz)
            .ok_or_else(|| format!("частота {hz} Гц не поддерживается (125/250/500/1000)"))?;
        let mut block = vec![0u8; POLLING_SIZE as usize];
        block[0] = CMD_POLLING;
        block[1] = POLLING_SIZE as u8;
        block[2] = self.profile_index;
        block[3] = code;
        block[4] = code ^ 0xFF;
        self.write_block(&block)?;
        let now = self.read_polling().unwrap_or(0);
        if now != hz {
            return Err(format!(
                "мышь вернула {now} Гц вместо {hz} — запись не применилась"
            ));
        }
        Ok(now)
    }

    pub fn read_firmware(&self) -> Option<String> {
        let raw = self.read_block(CMD_FIRMWARE, FIRMWARE_SIZE).ok()?;
        Some(
            raw.iter()
                .skip(2)
                .map(|b| format!("{b:02X}"))
                .collect::<Vec<_>>()
                .join(" "),
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn linear_codec_roundtrip() {
        for dpi in [400u32, 800, 1200, 1600, 2000, 2400, 3200, 6400, 8000] {
            let (x, y, double) = encode_dpi(dpi, Encoding::Linear);
            assert_eq!(decode_dpi(x, y, double, Encoding::Linear), dpi, "dpi={dpi}");
        }
    }

    #[test]
    fn linear_matches_x8_pro_documentation() {
        // «2000 DPI is 0x27 with the double bit clear»
        assert_eq!(encode_dpi(2000, Encoding::Linear), (0x27, 0, false));
        assert_eq!(decode_dpi(0x27, 0, false, Encoding::Linear), 2000);
        // 400*: (n+1)*50
        assert_eq!(encode_dpi(400, Encoding::Linear).0, 0x07);
    }

    #[test]
    fn step_table_matches_x8se_capture() {
        assert_eq!(encode_dpi(800, Encoding::StepTable), (0x12, 0, false));
        assert_eq!(encode_dpi(1600, Encoding::StepTable), (0x25, 0, false));
        assert_eq!(decode_dpi(0x12, 0, false, Encoding::StepTable), 800);
        assert_eq!(decode_dpi(0x25, 0, false, Encoding::StepTable), 1600);
        assert_eq!(decode_dpi(0x09, 0, false, Encoding::StepTable), 400);
    }

    #[test]
    fn high_dpi_uses_double_bit() {
        let (x, y, double) = encode_dpi(20_000, Encoding::Linear);
        assert!(double);
        assert_eq!(decode_dpi(x, y, double, Encoding::Linear), 20_000);
    }

    #[test]
    fn disabled_stage_is_zero() {
        assert_eq!(decode_dpi(0, 0, false, Encoding::Linear), 0);
        assert_eq!(decode_dpi(0, 0, false, Encoding::StepTable), 0);
    }

    #[test]
    fn encoding_picked_by_pid() {
        assert_eq!(Encoding::for_pid(0x2126), Encoding::Linear);
        assert_eq!(Encoding::for_pid(0xFA65), Encoding::Linear);
        assert_eq!(Encoding::for_pid(0x2120), Encoding::StepTable);
        assert_eq!(Encoding::parse("TABLE"), Some(Encoding::StepTable));
        assert_eq!(Encoding::parse("нет"), None);
    }

    fn sample_profile(encoding: Encoding) -> Profile {
        let mut raw = vec![0u8; RECEIVER_SIZE as usize];
        raw[0] = CMD_DPI;
        raw[1] = RECEIVER_SIZE as u8;
        raw[2] = 0x01;
        raw[5] = 0x3F;
        let stages = [400u32, 800, 1600, 1800, 2000, 2400];
        for (index, dpi) in stages.iter().enumerate() {
            let (x, y, _) = encode_dpi(*dpi, encoding);
            raw[OFF_STAGES + index] = x;
            raw[OFF_HIGH + index] = y;
        }
        raw[OFF_ACTIVE_STAGE] = 3;
        apply_checksum(&mut raw);
        parse_profile(&raw, encoding).unwrap()
    }

    #[test]
    fn parses_stage_table() {
        let profile = sample_profile(Encoding::Linear);
        assert_eq!(profile.stages[..6], [400, 800, 1600, 1800, 2000, 2400]);
        assert_eq!(profile.active_stage, 3);
    }

    #[test]
    fn set_dpi_updates_stage_and_checksum() {
        let mut profile = sample_profile(Encoding::Linear);
        assert!(set_active_dpi(&mut profile, 3200));
        assert_eq!(profile.stages[2], 3200);
        let stored = ((profile.raw[50] as u16) << 8) | profile.raw[51] as u16;
        assert_eq!(stored, checksum(&profile.raw));
    }

    #[test]
    fn stage_switch_keeps_values() {
        let mut profile = sample_profile(Encoding::Linear);
        assert!(set_active_stage(&mut profile, 5));
        assert_eq!(profile.active_stage, 5);
        assert_eq!(profile.stages[4], 2000);
        assert!(!set_active_stage(&mut profile, 9));
    }

    #[test]
    fn receiver_read_request_matches_capture() {
        // Официальный захват: чтение DPI
        // 04 0c a0 04 38 01 00 00 00 00 00 ed
        let frame = receiver_read_request(CMD_DPI, 0x38, 0x01);
        assert_eq!(
            frame,
            vec![0x04, 0x0C, 0xA0, 0x04, 0x38, 0x01, 0, 0, 0, 0, 0x00, 0xED]
        );
        // Чтение частоты опроса: 04 0c a0 06 09 01 ... c0
        let frame = receiver_read_request(CMD_POLLING, 0x09, 0x01);
        assert_eq!(frame[10], 0x00);
        assert_eq!(frame[11], 0xC0);
    }

    #[test]
    fn receiver_write_frame_structure() {
        let mut block = vec![0u8; 9];
        block[0] = CMD_POLLING;
        block[1] = 9;
        block[2] = 1;
        block[3] = 0x02;
        block[4] = 0xFD;
        let frame = receiver_write_frame(&block);
        assert_eq!(frame[0], 0x04);
        assert_eq!(frame[1], 14); // 9 + 5
        assert_eq!(frame[2], 0x00);
        let sum: u16 = frame[..frame.len() - 2]
            .iter()
            .map(|b| *b as u32)
            .sum::<u32>() as u16;
        assert_eq!(frame[frame.len() - 2], (sum >> 8) as u8);
        assert_eq!(frame[frame.len() - 1], (sum & 0xFF) as u8);
    }

    #[test]
    fn polling_codec() {
        for hz in [125u16, 250, 500, 1000] {
            assert_eq!(decode_polling(encode_polling(hz).unwrap()), Some(hz));
        }
        assert_eq!(encode_polling(8000), None);
        assert_eq!(decode_polling(0x33), None);
        assert_eq!(decode_polling(0x33 & 0x0F), None);
    }

    #[test]
    fn battery_and_dpi_events() {
        assert_eq!(
            parse_battery(&[0x03, 0x08, 0x40, 0x01, 0x4E]),
            Some((78, "от батареи"))
        );
        assert_eq!(
            parse_battery(&[0x03, 0x08, 0x40, 0x03, 0x59]),
            Some((89, "заряжается"))
        );
        assert_eq!(parse_battery(&[0x03, 0x02, 0x10, 0x03, 0x00]), None);
        assert_eq!(parse_dpi_event(&[0x03, 0x08, 0x10, 0x03, 0x00]), Some(3));
        assert_eq!(parse_dpi_event(&[0x03, 0x08, 0x40, 0x01, 0x4E]), None);
    }
}
