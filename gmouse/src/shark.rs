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

/// DPI -> байт сенсора. Линейная формула `dpi = (n + 1) * 50`,
/// для значений выше 12000 используется множитель 2 (бит в маске ступеней).
pub fn encode_dpi(dpi: u32) -> (u8, bool) {
    let dpi = dpi.clamp(50, 40_000);
    let double = dpi > 12_000;
    let effective = if double { dpi / 2 } else { dpi };
    let steps = (effective / 50).max(1);
    let n = steps.saturating_sub(1);
    (n.min(254) as u8, double)
}

/// Байт сенсора -> DPI.
pub fn decode_dpi(byte: u8, double: bool) -> u32 {
    let base = (byte as u32 + 1) * 50;
    if double {
        base * 2
    } else {
        base
    }
}

/// Частота опроса из байта: встречаются и индекс (0..3), и код (1/2/4/8).
pub fn decode_polling(byte: u8) -> Option<u16> {
    match byte {
        0x01 | 0x00 => Some(1000),
        0x02 => Some(500),
        0x04 => Some(250),
        0x08 => Some(125),
        3 => Some(2000),
        4 => Some(4000),
        5 => Some(8000),
        _ => None,
    }
}

pub fn encode_polling(hz: u16) -> Option<u8> {
    Some(match hz {
        125 => 0x08,
        250 => 0x04,
        500 => 0x02,
        1000 => 0x01,
        2000 => 3,
        4000 => 4,
        8000 => 5,
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
        let byte = *raw.get(off::STAGE_DPI + i)?;
        let high = *raw.get(off::STAGE_HIGH + i).unwrap_or(&0);
        let mask = *raw.get(off::STAGE_MASK_A).unwrap_or(&0);
        let double = high != 0 || (mask >> i) & 1 == 1;
        stages.push(decode_dpi(byte, double));
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
    let (byte, double) = encode_dpi(dpi);
    profile.raw[off::STAGE_DPI + index] = byte;
    if double {
        profile.raw[off::STAGE_MASK_A] |= 1 << index;
        profile.raw[off::STAGE_MASK_B] |= 1 << index;
        profile.raw[off::STAGE_HIGH + index] = 1;
    } else {
        profile.raw[off::STAGE_MASK_A] &= !(1 << index);
        profile.raw[off::STAGE_MASK_B] &= !(1 << index);
        profile.raw[off::STAGE_HIGH + index] = 0;
    }
    profile.stages[index] = decode_dpi(byte, double);
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

    pub fn read_polling(&self) -> Option<u16> {
        let raw = hid::read_feature_id(self.handle, REPORT_POLLING, POLLING_LEN)?;
        decode_polling(*raw.get(1)?)
    }

    pub fn write_polling(&self, hz: u16) -> Result<(), String> {
        let code = encode_polling(hz)
            .ok_or_else(|| format!("частота {hz} Гц не поддерживается"))?;
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
        for dpi in [400u32, 800, 1200, 1600, 1800, 2400, 3200, 6400] {
            let (byte, double) = encode_dpi(dpi);
            assert_eq!(decode_dpi(byte, double), dpi, "dpi={dpi}");
        }
    }

    #[test]
    fn dpi_above_12000_uses_double_bit() {
        let (byte, double) = encode_dpi(24000);
        assert!(double);
        assert_eq!(decode_dpi(byte, double), 24000);
    }

    #[test]
    fn dpi_matches_known_capture() {
        // Из публичной документации: байт 0x12 = 800 DPI, 0x25 = 1600 DPI
        assert_eq!(decode_dpi(0x12, false), 800);
        assert_eq!(decode_dpi(0x25, false), 1600);
        assert_eq!(encode_dpi(800).0, 0x12);
        assert_eq!(encode_dpi(1600).0, 0x25);
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
        assert_eq!(encode_polling(777), None);
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
