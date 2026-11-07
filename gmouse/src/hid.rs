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

const GENERIC_READ: u32 = 0x8000_0000;
const GENERIC_WRITE: u32 = 0x4000_0000;
const FILE_SHARE_WRITE: u32 = 0x02;
const OPEN_EXISTING: u32 = 3;

/// Открыть HID-интерфейс только на чтение.
///
/// Важно: с dwDesiredAccess = 0 Windows открывает устройство, но
/// HidD_GetProductString / HidD_GetManufacturerString / HidD_GetFeature
/// начинают возвращать ошибку — поэтому запрашиваем GENERIC_READ.
/// Если интерфейс занят (например, мышью управляет игра), пробуем
/// ещё раз без прав: VID/PID и дескриптор читаются и так.
fn open_path(path: &str) -> HANDLE {
    let wpath = wide(path);
    for access in [GENERIC_READ, 0] {
        let handle = unsafe {
            CreateFileW(
                wpath.as_ptr(),
                access,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                std::ptr::null_mut(),
                OPEN_EXISTING,
                0,
                0,
            )
        };
        if handle != INVALID_HANDLE_VALUE && handle != 0 {
            return handle;
        }
    }
    INVALID_HANDLE_VALUE
}

/// Открыть устройство для чтения/записи (нужно для репортов).
pub fn open_rw(path: &str) -> Option<HANDLE> {
    let wpath = wide(path);
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

/// Сколько интерфейсов SetupAPI вернул и сколько удалось открыть —
/// для команды `doctor`, чтобы отличить «нет устройств» от «нет прав».
#[derive(Debug, Default, Clone, Copy)]
pub struct EnumerateStats {
    pub interfaces: usize,
    pub opened: usize,
    pub errors: u32,
}

thread_local! {
    static LAST_STATS: std::cell::Cell<EnumerateStats> =
        const { std::cell::Cell::new(EnumerateStats {
            interfaces: 0,
            opened: 0,
            errors: 0,
        }) };
}

pub fn last_stats() -> EnumerateStats {
    LAST_STATS.with(|cell| cell.get())
}

/// Пройти по всем HID-интерфейсам и вернуть их пути (без открытия).
pub fn interface_paths() -> Vec<String> {
    let mut out = Vec::new();
    for_each_interface(|path| out.push(path));
    out
}

/// Проверить, открывается ли интерфейс. 0 — да, иначе код ошибки Win32.
pub fn probe_open(path: &str) -> u32 {
    let handle = open_path(path);
    if handle == INVALID_HANDLE_VALUE || handle == 0 {
        return unsafe { crate::win::GetLastError() };
    }
    close(handle);
    0
}

fn for_each_interface(mut callback: impl FnMut(String)) {

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
        if devs == INVALID_HANDLE_VALUE || devs == 0 {
            return;
        }
        let mut index = 0u32;
        loop {
            let mut data = SP_DEVICE_INTERFACE_DATA {
                cbSize: std::mem::size_of::<SP_DEVICE_INTERFACE_DATA>() as u32,
                InterfaceClassGuid: guid,
                Flags: 0,
                Reserved: 0,
            };
            if SetupDiEnumDeviceInterfaces(
                devs,
                std::ptr::null_mut(),
                &guid,
                index,
                &mut data,
            ) == 0
            {
                break;
            }
            index += 1;

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
            // SP_DEVICE_INTERFACE_DETAIL_DATA_W:
            //   DWORD cbSize;  WCHAR DevicePath[1];
            // sizeof = 8 на x64 (4 + 2 + выравнивание), 6 на x86,
            // НО DevicePath всегда начинается с offset = 4 (сразу за cbSize).
            // Если взять offset = sizeof, путь теряет первые два символа
            // («\\?\» -> «?\») и CreateFileW возвращает ERROR_INVALID_NAME (123).
            let size_of_detail: u32 =
                if cfg!(target_pointer_width = "64") { 8 } else { 6 };
            let path_offset = 4usize;
            let mut buf = vec![0u8; required as usize];
            buf[0..4].copy_from_slice(&size_of_detail.to_le_bytes());
            if SetupDiGetDeviceInterfaceDetailW(
                devs,
                &data,
                buf.as_mut_ptr() as *mut std::ffi::c_void,
                required,
                &mut required,
                std::ptr::null_mut(),
            ) == 0
            {
                continue;
            }
            let path_units: Vec<u16> = buf[path_offset..]
                .chunks_exact(2)
                .map(|c| u16::from_le_bytes([c[0], c[1]]))
                .collect();
            let path = from_wide(&path_units);
            if !path.is_empty() {
                callback(path);
            }
        }
        SetupDiDestroyDeviceInfoList(devs);
    }
}

/// Все HID-интерфейсы в системе (с открытием для чтения атрибутов).
pub fn enumerate() -> Vec<HidInfo> {
    let mut found = Vec::new();
    let mut stats = EnumerateStats::default();
    for_each_interface(|path| {
        stats.interfaces += 1;
        match describe(&path) {
            Some(info) => {
                stats.opened += 1;
                found.push(info);
            }
            None => stats.errors += 1,
        }
    });
    LAST_STATS.with(|cell| cell.set(stats));
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

    // «Родитель» = путь без хвоста #{guid} и без &colNN: по нему
    // группируются интерфейсы одной физической мыши
    // (vid_1a2c&pid_2511&mi_02 и vid_1a2c&pid_2511&mi_02&col01 — одно устройство).
    info.parent = device_key(path);

    // Устройство без VID/PID всё равно показываем: путь и HID-caps
    // помогают понять, что происходит (бывает у Bluetooth-интерфейсов).
    Some(info)
}

/// Ключ физического устройства из пути HID-интерфейса.
/// "\\?\hid#vid_1a2c&pid_2511&mi_02&col01#8&1169eeba&0&0000#{guid}"
///   -> "vid_1a2c&pid_2511&mi_02"
pub fn device_key(path: &str) -> String {
    let without_guid = match path.rfind("#{") {
        Some(index) => &path[..index],
        None => path,
    };
    let interface_part = without_guid
        .rsplit_once('#')
        .map(|(head, _)| head)
        .unwrap_or(without_guid);
    let name = interface_part
        .rsplit_once('#')
        .map(|(_, tail)| tail)
        .unwrap_or(interface_part);
    match name.find("&col") {
        Some(index) => name[..index].to_string(),
        None => name.to_string(),
    }
}

/// Прочитать feature-репорт конкретного Report ID.
pub fn read_feature_id(handle: HANDLE, report_id: u8, length: u16) -> Option<Vec<u8>> {
    if length < 2 {
        return None;
    }
    let mut buf = vec![0u8; length as usize];
    buf[0] = report_id;
    unsafe {
        if HidD_GetFeature(handle, buf.as_mut_ptr() as *mut std::ffi::c_void,
                           length as u32) == 0 {
            return None;
        }
    }
    Some(buf)
}

/// Записать output-репорт (первый байт — Report ID).
pub fn write_output_report(handle: HANDLE, data: &[u8]) -> Option<()> {
    if data.is_empty() {
        return None;
    }
    let mut buf = vec![0u8; data.len().max(9)];
    buf[..data.len()].copy_from_slice(data);
    unsafe {
        if HidD_SetOutputReport(
            handle,
            buf.as_ptr() as *const std::ffi::c_void,
            buf.len() as u32,
        ) == 0
        {
            None
        } else {
            Some(())
        }
    }
}

/// Прочитать входной репорт с таймаутом через overlapped-ввод.
/// Возвращает None, если данных нет за отведённое время.
pub fn read_input_timeout(handle: HANDLE, length: usize, timeout_ms: u32) -> Option<Vec<u8>> {
    if length == 0 || length > 4096 {
        return None;
    }
    #[repr(C)]
    struct OVERLAPPED {
        internal: usize,
        internal_high: usize,
        offset: u32,
        offset_high: u32,
        event: HANDLE,
    }
    #[link(name = "kernel32")]
    extern "system" {
        fn CreateEventW(attrs: *mut std::ffi::c_void, manual: BOOL,
                        initial: BOOL, name: LPCWSTR) -> HANDLE;
        fn WaitForSingleObject(handle: HANDLE, ms: u32) -> u32;
        fn ResetEvent(handle: HANDLE) -> BOOL;
        fn CancelIo(handle: HANDLE) -> BOOL;
    }
    const WAIT_OBJECT_0: u32 = 0;
    let mut buf = vec![0u8; length];
    let mut read = 0u32;
    unsafe {
        let event = CreateEventW(std::ptr::null_mut(), 1, 0, std::ptr::null());
        if event == 0 {
            return None;
        }
        let mut overlapped = OVERLAPPED {
            internal: 0,
            internal_high: 0,
            offset: 0,
            offset_high: 0,
            event,
        };
        let started = ReadFile(
            handle,
            buf.as_mut_ptr() as *mut std::ffi::c_void,
            length as u32,
            &mut read,
            &mut overlapped as *mut OVERLAPPED as *mut std::ffi::c_void,
        );
        let ok = if started != 0 {
            read as usize
        } else {
            let wait = WaitForSingleObject(event, timeout_ms);
            if wait != WAIT_OBJECT_0 {
                CancelIo(handle);
                CloseHandle(event);
                return None;
            }
            let mut transferred = 0u32;
            GetOverlappedResult(handle, &mut overlapped as *mut OVERLAPPED
                as *mut std::ffi::c_void, &mut transferred, 0);
            ResetEvent(event);
            transferred as usize
        };
        CloseHandle(event);
        if ok == 0 {
            return None;
        }
        buf.truncate(ok);
        Some(buf)
    }
}

/// Записать feature-репорт (первый байт — Report ID).
pub fn write_feature(handle: HANDLE, data: &[u8]) -> Option<()> {
    if data.len() < 2 {
        return None;
    }
    unsafe {
        if HidD_SetFeature(
            handle,
            data.as_ptr() as *const std::ffi::c_void,
            data.len() as u32,
        ) == 0
        {
            None
        } else {
            Some(())
        }
    }
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn device_key_groups_interfaces() {
        let a = "\\\\?\\hid#vid_1a2c&pid_2511&mi_02&col01#8&1169eeba&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
        let b = "\\\\?\\hid#vid_1a2c&pid_2511&mi_02&col04#8&1169eeba&0&0003#{4d1e55b2-f16f-11cf-88cb-001111000030}";
        let c = "\\\\?\\hid#vid_1a2c&pid_2511&mi_01#8&292d7aff&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
        assert_eq!(device_key(a), "vid_1a2c&pid_2511&mi_02");
        assert_eq!(device_key(a), device_key(b));
        assert_ne!(device_key(a), device_key(c));
    }

    #[test]
    fn stats_default_is_zero() {
        let stats = EnumerateStats::default();
        assert_eq!(stats.interfaces, 0);
        assert_eq!(stats.opened, 0);
        assert_eq!(stats.errors, 0);
    }

    #[test]
    fn probe_bad_path_reports_error() {
        // Несуществующий путь: Windows обязана вернуть ненулевой код ошибки
        let code = probe_open("\\\\?\\hid#vid_0000&pid_0000#none");
        // На Windows CreateFileW вернёт ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND
        assert_ne!(code, 0);
    }

    #[test]
    fn capabilities_have_expected_layout() {
        // Раскладка должна совпадать с hidpi.h:
        // HIDP_CAPS = 2+2 + 2*3 + 2*17 + 2*10 = 64 байта
        assert_eq!(std::mem::size_of::<HIDP_CAPS>(), 64);
        assert_eq!(std::mem::size_of::<HIDD_ATTRIBUTES>(), 12);
        assert_eq!(
            std::mem::size_of::<SP_DEVICE_INTERFACE_DATA>(),
            if cfg!(target_pointer_width = "64") { 32 } else { 28 }
        );
    }
}

