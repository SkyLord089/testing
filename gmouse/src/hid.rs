//! Перечисление HID-устройств через SetupAPI + hid.dll.
//!
//! Нужно для двух вещей:
//!   1) показать, что за мышь подключена (VID/PID, пути, HID usage);
//!   2) диагностика: прочитать все входные репорты и понять, какие байты
//!      меняются при движении/кликах — это основа для поиска протокола DPI
//!      у «безымянных» контроллеров.

use crate::win::*;
use std::ffi::{OsStr, OsString};
use std::os::windows::ffi::{OsStrExt, OsStringExt};

#[derive(Debug, Clone)]
pub struct HidInfo {
    pub path: String,
    pub vendor_id: u16,
    pub product_id: u16,
    pub version: u16,
    pub product: String,
    pub manufacturer: String,
    pub serial: String,
    pub usage_page: u16,
    pub usage: u16,
    pub input_len: u16,
    pub output_len: u16,
    pub feature_len: u16,
    /// Верхний уровень (для группировки интерфейсов одного устройства)
    pub parent: String,
}

impl HidInfo {
    pub fn vid_pid(&self) -> String {
        format!("{:04X}:{:04X}", self.vendor_id, self.product_id)
    }

    pub fn is_mouse(&self) -> bool {
        // HIDP_CAPS отдаёт верхнюю коллекцию (Top Level Collection):
        // 0x01/0x02 — настоящий указатель.
        self.usage_page == 0x01 && self.usage == 0x02
    }

    pub fn is_vendor_defined(&self) -> bool {
        self.usage_page >= 0xFF00
    }

    pub fn kind(&self) -> &'static str {
        match (self.usage_page, self.usage) {
            (0x01, 0x02) => "мышь",
            (0x01, 0x06) => "клавиатура",
            (0x01, 0x00) => "указатель/прочее",
            (0x0C, _) => "потребительский (мультимедиа)",
            (0xFF00..=0xFFFF, _) => "vendor-defined (служебный)",
            (0x0D, _) => "дигитайзер",
            _ => "иное",
        }
    }
}

fn wide(s: &str) -> Vec<u16> {
    OsStr::new(s).encode_wide().chain(std::iter::once(0)).collect()
}

fn from_wide(buf: &[u16]) -> String {
    let end = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
    OsString::from_wide(&buf[..end]).to_string_lossy().to_string()
}

fn open_path(path: &str) -> HANDLE {
    let wpath = wide(path);
    const FILE_SHARE_WRITE: u32 = 0x02;
    const OPEN_EXISTING: u32 = 3;
    unsafe {
        CreateFileW(
            wpath.as_ptr(),
            0, // без прав: достаточно для атрибутов
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            std::ptr::null_mut(),
            OPEN_EXISTING,
            0,
            0,
        )
    }
}

/// Открыть устройство для чтения/записи (нужно для репортов).
pub fn open_rw(path: &str) -> Option<HANDLE> {
    let wpath = wide(path);
    const GENERIC_READ: u32 = 0x8000_0000;
    const GENERIC_WRITE: u32 = 0x4000_0000;
    const FILE_SHARE_WRITE: u32 = 0x02;
    const OPEN_EXISTING: u32 = 3;
    let handle = unsafe {
        CreateFileW(
            wpath.as_ptr(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            std::ptr::null_mut(),
            OPEN_EXISTING,
            0,
            0,
        )
    };
    if handle == INVALID_HANDLE_VALUE || handle == 0 {
        None
    } else {
        Some(handle)
    }
}

pub fn close(handle: HANDLE) {
    unsafe {
        CloseHandle(handle);
    }
}

/// Все HID-интерфейсы в системе.
pub fn enumerate() -> Vec<HidInfo> {
    let mut found = Vec::new();
    unsafe {
        let mut guid = GUID {
            Data1: 0,
            Data2: 0,
            Data3: 0,
            Data4: [0; 8],
        };
        HidD_GetHidGuid(&mut guid);

        let devs = SetupDiGetClassDevsW(
            &guid,
            std::ptr::null(),
            0,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE,
        );
        if devs == INVALID_HANDLE_VALUE {
            return found;
        }

        let mut index = 0u32;
        loop {
            let mut data = SP_DEVICE_INTERFACE_DATA {
                cbSize: std::mem::size_of::<SP_DEVICE_INTERFACE_DATA>() as u32,
                InterfaceClassGuid: guid,
                Flags: 0,
                Reserved: 0,
            };
            let ok = SetupDiEnumDeviceInterfaces(
                devs,
                std::ptr::null_mut(),
                &guid,
                index,
                &mut data,
            );
            if ok == 0 {
                break;
            }
            index += 1;

            // первый вызов — узнать размер буфера
            let mut required = 0u32;
            SetupDiGetDeviceInterfaceDetailW(
                devs,
                &data,
                std::ptr::null_mut(),
                0,
                &mut required,
                std::ptr::null_mut(),
            );
            if required == 0 {
                continue;
            }
            // SP_DEVICE_INTERFACE_DETAIL_DATA_W: DWORD cbSize + WCHAR Path[1].
            // Размер cbSize зависит от выравнивания: 8 на x64, 6 на x86.
            let size_of_detail: u32 = if cfg!(target_pointer_width = "64") { 8 } else { 6 };
            let path_offset = size_of_detail as usize;
            let mut buf = vec![0u8; required as usize];
            buf[0..4].copy_from_slice(&size_of_detail.to_le_bytes());
            let ok = SetupDiGetDeviceInterfaceDetailW(
                devs,
                &data,
                buf.as_mut_ptr() as *mut std::ffi::c_void,
                required,
                &mut required,
                std::ptr::null_mut(),
            );
            if ok == 0 {
                continue;
            }
            let path_units: Vec<u16> = buf[path_offset..]
                .chunks_exact(2)
                .map(|c| u16::from_le_bytes([c[0], c[1]]))
                .collect();
            let path = from_wide(&path_units);
            if path.is_empty() {
                continue;
            }
            if let Some(info) = describe(&path) {
                found.push(info);
            }
        }
        SetupDiDestroyDeviceInfoList(devs);
    }
    found
}

fn describe(path: &str) -> Option<HidInfo> {
    let handle = open_path(path);
    if handle == INVALID_HANDLE_VALUE || handle == 0 {
        return None;
    }

    let mut attrs = HIDD_ATTRIBUTES {
        Size: std::mem::size_of::<HIDD_ATTRIBUTES>() as u32,
        VendorID: 0,
        ProductID: 0,
        VersionNumber: 0,
    };
    let mut info = HidInfo {
        path: path.to_string(),
        vendor_id: 0,
        product_id: 0,
        version: 0,
        product: String::new(),
        manufacturer: String::new(),
        serial: String::new(),
        usage_page: 0,
        usage: 0,
        input_len: 0,
        output_len: 0,
        feature_len: 0,
        parent: String::new(),
    };

    unsafe {
        if HidD_GetAttributes(handle, &mut attrs) != 0 {
            info.vendor_id = attrs.VendorID;
            info.product_id = attrs.ProductID;
            info.version = attrs.VersionNumber;
        }
        let mut buf = [0u16; 256];
        let raw = buf.as_mut_ptr() as *mut std::ffi::c_void;
        let size = (buf.len() * 2) as u32;
        if HidD_GetProductString(handle, raw, size) != 0 {
            info.product = from_wide(&buf);
        }
        if HidD_GetManufacturerString(handle, raw, size) != 0 {
            info.manufacturer = from_wide(&buf);
        }
        if HidD_GetSerialNumberString(handle, raw, size) != 0 {
            info.serial = from_wide(&buf);
        }

        let mut preparsed: *mut std::ffi::c_void = std::ptr::null_mut();
        if HidD_GetPreparsedData(handle, &mut preparsed) != 0 {
            let mut caps = HIDP_CAPS::default();
            if HidP_GetCaps(preparsed, &mut caps) == HIDP_STATUS_SUCCESS {
                // У HIDP_CAPS.Usage/UsagePage лежит ВЕРХНЯЯ коллекция
                // (Top Level Collection): 0x01/0x02 — мышь, 0x01/0x06 —
                // клавиатура, 0x0C/0x01 — мультимедиа, 0xFF00.. — vendor.
                info.usage_page = caps.UsagePage;
                info.usage = caps.Usage;
                info.input_len = caps.InputReportByteLength;
                info.output_len = caps.OutputReportByteLength;
                info.feature_len = caps.FeatureReportByteLength;
            }
            HidD_FreePreparsedData(preparsed);
        }
    }
    close(handle);

    // Родитель: часть пути до последнего '\' — позволяет связать
    // несколько HID-интерфейсов одной физической мыши.
    info.parent = path
        .rsplit_once('\\')
        .map(|(head, _)| head.to_string())
        .unwrap_or_else(|| path.to_string());

    if info.vendor_id == 0 && info.product_id == 0 {
        return None;
    }
    Some(info)
}

/// Прочитать один входной репорт (блокирующе, с таймаутом).
/// Возвращает (report_id, данные).
pub fn read_input(handle: HANDLE, len: usize) -> Option<Vec<u8>> {
    if len == 0 || len > 4096 {
        return None;
    }
    let mut buf = vec![0u8; len];
    let mut read = 0u32;
    unsafe {
        // ReadFile на HID-устройстве блокируется до появления данных;
        // для диагностики это нормально — выходим по Ctrl+C.
        let ok = ReadFile(
            handle,
            buf.as_mut_ptr() as *mut std::ffi::c_void,
            len as u32,
            &mut read,
            std::ptr::null_mut(),
        );
        if ok == 0 {
            return None;
        }
    }
    buf.truncate(read as usize);
    Some(buf)
}

/// Отправить feature-репорт и вернуть ответ (некоторые контроллеры
/// принимают настройки именно так).
pub fn feature_request(handle: HANDLE, data: &[u8]) -> Option<Vec<u8>> {
    if data.is_empty() {
        return None;
    }
    let mut buf = vec![0u8; data.len().max(9)];
    buf[..data.len()].copy_from_slice(data);
    unsafe {
        let ok = HidD_SetFeature(
            handle,
            buf.as_ptr() as *const std::ffi::c_void,
            buf.len() as u32,
        );
        if ok == 0 {
            return None;
        }
        let mut reply = buf.clone();
        let ok = HidD_GetFeature(
            handle,
            reply.as_mut_ptr() as *mut std::ffi::c_void,
            reply.len() as u32,
        );
        if ok == 0 {
            return None;
        }
        Some(reply)
    }
}

/// Записать output-репорт.
pub fn output_report(handle: HANDLE, data: &[u8]) -> bool {
    if data.is_empty() {
        return false;
    }
    let mut buf = vec![0u8; data.len().max(9)];
    buf[..data.len()].copy_from_slice(data);
    unsafe {
        HidD_SetOutputReport(
            handle,
            buf.as_ptr() as *const std::ffi::c_void,
            buf.len() as u32,
        ) != 0
    }
}
