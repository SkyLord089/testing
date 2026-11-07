//! Протокол Attack Shark (Beken BK3630): X8 Plus, X8 Pro, X8 SE, X11.
//!
//! Основан на публичных реверс-инженерных материалах сообщества
//! (OpenMouse / attackshark-bridge / ATKsoft): формат feature-репортов,
//! кодирование DPI и события батареи.
//!
//! Кратко:
//!   * служебный интерфейс — interface 2; настройки ходят feature-репортами,
//!     где Report ID = команда: 0x04 — профиль (DPI, активная ступень, LOD),
//!     0x06 — частота опроса, 0x0A — дамп состояния, 0xA0 — канал чтения;
//!   * DPI кодируется как `dpi = (n + 1) * 50`, при >12000 включается
//!     бит «x2» в маске ступеней;
//!   * запись = чтение-изменение-запись: остальные байты профиля нужно
//!     сохранять как есть, иначе собьются цвета подсветки и прочие поля;
//!   * в конце профиля 16-битная контрольная сумма по байтам 3..49.

use crate::hid;
use crate::win::HANDLE;

pub const REPORT_PROFILE: u8 = 0x04;
pub const REPORT_POLLING: u8 = 0x06;
pub const REPORT_STATE: u8 = 0x0A;
pub const REPORT_READ_CHANNEL: u8 = 0xA0;

/// Размер feature-репорта профиля вместе с Report ID.
pub const PROFILE_LEN: u16 = 52;
pub const POLLING_LEN: u16 = 9;

/// Смещения в репорте 0x04 (см. §3 документации протокола).
mod off {
    pub const ANGLE_SNAP: usize = 3;
    pub const RIPPLE: usize = 4;
    pub const STAGE_MASK_A: usize = 6;
    pub const STAGE_MASK_B: usize = 7;
    pub const STAGE_DPI: usize = 8; // 6 байт: ступени 1..6
    pub const STAGE_HIGH: usize = 16; // 6 байт: «x2»
    pub const ACTIVE_STAGE: usize = 24; // 1..6
    pub const CHECKSUM: usize = 50; // 2 байта, big-endian
}

#[derive(Debug, Clone, PartialEq)]
pub struct Profile {
    pub stages: Vec<u32>,
    pub active_stage: u8,
    pub angle_snap: bool,
    pub ripple_control: bool,
    pub polling_hz: Option<u16>,
    pub raw: Vec<u8>,
}

/// Таблица ступеней сенсора PAW3311 (как в публичной документации
/// attackshark-bridge / X11 driver): DPI = (индекс + 1) * 50 при yByte = 0
/// и (индекс + 1) * 100 при yByte = 1. Не равномерная — поэтому и нельзя
/// считать «(n+1)*50» напрямую по байту.
const DPI_3311: [u8; 200] = [
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0e,
    0x0f, 0x10, 0x11, 0x12, 0x13, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1b, 0x1c,
    0x1d, 0x1e, 0x1f, 0x20, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x29, 0x2a,
    0x2b, 0x2c, 0x2d, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x3b, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x43, 0x44, 0x45, 0x46,
    0x47, 0x48, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x51, 0x52, 0x53, 0x54,
    0x55, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5e, 0x5f, 0x60, 0x61, 0x62,
    0x63, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70,
    0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7f,
    0x80, 0x81, 0x82, 0x83, 0x84, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8d,
    0x8e, 0x8f, 0x90, 0x91, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x9a, 0x9b,
    0x9c, 0x9d, 0x9e, 0x9f, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa7, 0xa8, 0xa9,
    0xaa, 0xab, 0xac, 0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xc0, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xcf, 0xd0, 0xd1, 0xd2, 0xd3,
    0xd4, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdd, 0xde, 0xdf, 0xe0, 0xe1,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xea, 0xeb,
];

/// Минимальный и максимальный DPI, которые кодируются и читаются обратно
/// без потерь (выше 20000 кодирование в прошивке другое).
pub const MIN_DPI: u32 = 50;
pub const MAX_DPI: u32 = 20_000;

/// DPI -> (xByte, yByte, double).
pub fn encode_dpi(dpi: u32) -> (u8, u8, bool) {
    let mut dpi = dpi.clamp(MIN_DPI, MAX_DPI);
    let double = dpi > 10_000;
    if double {
        // выше 10000 сенсор хранит половину значения
        dpi = (dpi as f64 / 2.0).round() as u32;
    }
    if dpi > 5_000 && dpi % 100 == 0 {
        let index = (dpi / 100 - 1) as usize;
        return (DPI_3311[index.min(DPI_3311.len() - 1)], 1, double);
    }
    let index = ((dpi.saturating_sub(50)) / 50) as usize;
    (DPI_3311[index.min(DPI_3311.len() - 1)], 0, double)
}

/// (xByte, yByte, double) -> DPI. 0 означает «ступень выключена».
pub fn decode_dpi(x_byte: u8, y_byte: u8, double: bool) -> u32 {
    if x_byte == 0 && y_byte == 0 {
        return 0;
    }
    let position = DPI_3311.iter().position(|b| *b == x_byte);
    let mut dpi = match (y_byte, position) {
        (0, Some(index)) => index as u32 * 50 + 50,
        (1, Some(index)) => index as u32 * 100 + 100,
        (_, Some(_)) => 10_100 + (((y_byte as u32) << 8 | x_byte as u32) - 199) * 100,
        // байта нет в таблице — показываем хотя бы грубую оценку
        (0, None) => x_byte as u32 * 50,
        _ => 0,
    };
    if double {
        dpi = dpi.saturating_mul(2);
    }
    dpi
}

/// Частота опроса из байта: встречаются и индекс (0..3), и код (1/2/4/8).
pub fn decode_polling(byte: u8) -> Option<u16> {
    match byte {
        0x01 | 0x00 => Some(1000),
        0x02 => Some(500),
        0x04 => Some(250),
        0x08 => Some(125),
        // 2000/4000/8000 Гц кодируются по-другому; кодировка не подтверждена
        _ => None,
    }
}

pub fn encode_polling(hz: u16) -> Option<u8> {
    Some(match hz {
        125 => 0x08,
        250 => 0x04,
        500 => 0x02,
        1000 => 0x01,
        // выше 1000 Гц протокол другой — не пишем наугад
        _ => return None,
    })
}

/// Контрольная сумма профиля: сумма байт 3..49, big-endian в 50..51.
pub fn checksum(data: &[u8]) -> u16 {
    let end = off::CHECKSUM.min(data.len());
    let mut sum: u32 = 0;
    for byte in &data[3.min(end)..end] {
        sum = sum.wrapping_add(*byte as u32);
    }
    (sum & 0xFFFF) as u16
}

pub fn apply_checksum(data: &mut [u8]) -> bool {
    if data.len() < off::CHECKSUM + 2 {
        return false;
    }
    let sum = checksum(data);
    data[off::CHECKSUM] = (sum >> 8) as u8;
    data[off::CHECKSUM + 1] = (sum & 0xFF) as u8;
    true
}

pub fn parse_profile(raw: &[u8]) -> Option<Profile> {
    if raw.len() < off::ACTIVE_STAGE + 1 {
        return None;
    }
    let mut stages = Vec::with_capacity(6);
    for i in 0..6 {
        let x_byte = *raw.get(off::STAGE_DPI + i)?;
        let y_byte = *raw.get(off::STAGE_HIGH + i).unwrap_or(&0);
        // бит «x2» в маске ступеней ставит прошивка для значений >10000 DPI
        let double = (raw.get(off::STAGE_MASK_A).unwrap_or(&0) >> i) & 1 == 1;
        stages.push(decode_dpi(x_byte, y_byte, double));
    }
    Some(Profile {
        stages,
        active_stage: raw[off::ACTIVE_STAGE].clamp(1, 6),
        angle_snap: raw[off::ANGLE_SNAP] != 0,
        ripple_control: raw[off::RIPPLE] != 0,
        polling_hz: None,
        raw: raw.to_vec(),
    })
}

/// Установить DPI активной ступени в уже прочитанном профиле.
pub fn set_active_dpi(profile: &mut Profile, dpi: u32) -> bool {
    if profile.raw.len() < PROFILE_LEN as usize {
        return false;
    }
    let index = (profile.active_stage.saturating_sub(1) as usize).min(5);
    let (x_byte, y_byte, double) = encode_dpi(dpi);
    profile.raw[off::STAGE_DPI + index] = x_byte;
    profile.raw[off::STAGE_HIGH + index] = y_byte;
    if double {
        profile.raw[off::STAGE_MASK_A] |= 1 << index;
        profile.raw[off::STAGE_MASK_B] |= 1 << index;
    } else {
        profile.raw[off::STAGE_MASK_A] &= !(1 << index);
        profile.raw[off::STAGE_MASK_B] &= !(1 << index);
    }
    profile.stages[index] = decode_dpi(x_byte, y_byte, double);
    apply_checksum(&mut profile.raw)
}

pub fn set_active_stage(profile: &mut Profile, stage: u8) -> bool {
    if !(1..=6).contains(&stage) || profile.raw.len() <= off::ACTIVE_STAGE {
        return false;
    }
    profile.raw[off::ACTIVE_STAGE] = stage;
    profile.active_stage = stage;
    apply_checksum(&mut profile.raw)
}

/// Обмен с мышью через feature-репорты.
pub struct Device {
    pub handle: HANDLE,
    pub path: String,
}

impl Device {
    pub fn read_profile(&self) -> Result<Profile, String> {
        let raw = hid::read_feature_id(self.handle, REPORT_PROFILE, PROFILE_LEN)
            .ok_or_else(|| {
                "не удалось прочитать профиль (Report 0x04). \
                 Запустите gmouse от администратора или закройте фирменную утилиту"
                    .to_string()
            })?;
        parse_profile(&raw).ok_or_else(|| "профиль короче ожидаемого".to_string())
    }

    pub fn write_profile(&self, profile: &Profile) -> Result<(), String> {
        let mut data = profile.raw.clone();
        if !apply_checksum(&mut data) {
            return Err("профиль слишком короткий для записи".into());
        }
        if data.len() < PROFILE_LEN as usize {
            data.resize(PROFILE_LEN as usize, 0);
        }
        hid::write_feature(self.handle, &data)
            .ok_or_else(|| "мышь отклонила запись профиля".to_string())
    }

    /// Прочитать дамп состояния (Report 0x0A) через канал 0xA0:
    /// сначала просим, ждём готовности, затем читаем сам репорт.
    pub fn read_state(&self) -> Option<Vec<u8>> {
        let mut request = vec![0u8; 8];
        request[0] = REPORT_READ_CHANNEL;
        request[1] = REPORT_STATE;
        request[2] = 64; // Windows отдаёт максимум 64 байта feature-репорта
        hid::write_feature(self.handle, &request)?;
        for _ in 0..20 {
            std::thread::sleep(std::time::Duration::from_millis(30));
            let reply = hid::read_feature_id(self.handle, REPORT_READ_CHANNEL, 8)?;
            if reply.get(1).copied().unwrap_or(0) == 1 {
                return hid::read_feature_id(self.handle, REPORT_STATE, 64);
            }
        }
        None
    }

    pub fn read_polling(&self) -> Option<u16> {
        let raw = hid::read_feature_id(self.handle, REPORT_POLLING, POLLING_LEN)?;
        decode_polling(*raw.get(1)?)
    }

    pub fn write_polling(&self, hz: u16) -> Result<(), String> {
        let code = encode_polling(hz).ok_or_else(|| {
            format!("частота {hz} Гц не поддерживается (только 125/250/500/1000)")
        })?;
        let mut data = vec![0u8; POLLING_LEN as usize];
        data[0] = REPORT_POLLING;
        data[1] = code;
        data[2] = code ^ 0xFF;
        hid::write_feature(self.handle, &data)
            .ok_or_else(|| "мышь отклонила смену частоты опроса".to_string())
    }
}

/// Разбор асинхронного события батареи: `03 <dev> 40 <state> <percent>`.
pub fn parse_battery(event: &[u8]) -> Option<(u8, &'static str)> {
    if event.len() < 5 || event[0] != 0x03 || !matches!(event[2], 0x40 | 0x41) {
        return None;
    }
    let percent = event[4].min(100);
    let state = match event[3] {
        0x01 => "заряжено",
        0x02 => "полностью заряжено",
        0x03 => "заряжается",
        _ => "от батареи",
    };
    Some((percent, state))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dpi_codec_roundtrip() {
        for dpi in [
            400u32, 800, 1200, 1600, 1800, 2000, 2400, 3200, 6400, 8000, 12000,
        ] {
            let (x, y, double) = encode_dpi(dpi);
            assert_eq!(decode_dpi(x, y, double), dpi, "dpi={dpi}");
        }
    }

    #[test]
    fn dpi_above_10000_uses_double_bit() {
        let (x, y, double) = encode_dpi(20000);
        assert!(double);
        assert_eq!(decode_dpi(x, y, double), 20000);
    }

    #[test]
    fn dpi_matches_known_capture() {
        // Публичная таблица ступеней PAW3311: 0x12 = 800, 0x25 = 1600
        assert_eq!(decode_dpi(0x12, 0, false), 800);
        assert_eq!(decode_dpi(0x25, 0, false), 1600);
        assert_eq!(encode_dpi(800), (0x12, 0, false));
        assert_eq!(encode_dpi(1600), (0x25, 0, false));
        // 0x09 = 400 DPI (первая ступень из захвата)
        assert_eq!(decode_dpi(0x09, 0, false), 400);
    }

    #[test]
    fn disabled_stage_is_zero() {
        assert_eq!(decode_dpi(0, 0, false), 0);
    }

    #[test]
    fn checksum_is_big_endian_sum() {
        let mut data = vec![0u8; PROFILE_LEN as usize];
        data[0] = 0x04;
        for (i, byte) in data.iter_mut().enumerate().take(49).skip(3) {
            *byte = (i % 7) as u8;
        }
        assert!(apply_checksum(&mut data));
        let stored = ((data[50] as u16) << 8) | data[51] as u16;
        assert_eq!(stored, checksum(&data));
    }

    fn sample_profile() -> Profile {
        let mut raw = vec![0u8; PROFILE_LEN as usize];
        raw[0] = REPORT_PROFILE;
        raw[1] = 0x38;
        raw[2] = 0x01;
        raw[5] = 0x3F;
        raw[off::STAGE_DPI..off::STAGE_DPI + 6]
            .copy_from_slice(&[0x09, 0x12, 0x25, 0x2A, 0x2F, 0x38]);
        raw[off::ACTIVE_STAGE] = 3;
        apply_checksum(&mut raw);
        parse_profile(&raw).unwrap()
    }

    #[test]
    fn parses_documented_stage_table() {
        let profile = sample_profile();
        assert_eq!(profile.stages, vec![400, 800, 1600, 1800, 2000, 2400]);
        assert_eq!(profile.active_stage, 3);
    }

    #[test]
    fn sets_dpi_of_active_stage_and_fixes_checksum() {
        let mut profile = sample_profile();
        assert!(set_active_dpi(&mut profile, 3200));
        // активная ступень 3
        assert_eq!(profile.stages[2], 3200);
        let stored = ((profile.raw[50] as u16) << 8) | profile.raw[51] as u16;
        assert_eq!(stored, checksum(&profile.raw));
    }

    #[test]
    fn stage_switch_keeps_stage_values() {
        let mut profile = sample_profile();
        assert!(set_active_stage(&mut profile, 2));
        assert_eq!(profile.active_stage, 2);
        assert_eq!(profile.stages[1], 800);
        assert!(!set_active_stage(&mut profile, 9));
    }

    #[test]
    fn polling_codec() {
        for hz in [125u16, 250, 500, 1000] {
            assert_eq!(decode_polling(encode_polling(hz).unwrap()), Some(hz));
        }
        assert_eq!(decode_polling(0x02), Some(500));
        // неподтверждённые частоты не кодируем
        assert_eq!(encode_polling(777), None);
        assert_eq!(encode_polling(8000), None);
    }

    #[test]
    fn battery_event() {
        assert_eq!(
            parse_battery(&[0x03, 0x02, 0x40, 0x01, 0x64]),
            Some((100, "заряжено"))
        );
        assert_eq!(
            parse_battery(&[0x03, 0x02, 0x40, 0x03, 0x5E]),
            Some((94, "заряжается"))
        );
        assert_eq!(parse_battery(&[0x03, 0x02, 0x10, 0x01, 0x64]), None);
        assert_eq!(parse_battery(&[0x03]), None);
    }
}
