//! Прямые привязки к Win32 API без сторонних крейтов.
//!
//! Здесь собрано ровно то, что нужно программе:
//!   * хуки мыши и клавиатуры (`SetWindowsHookExW`) — переназначение кнопок;
//!   * `SendInput` — воспроизведение кликов и прокрутки;
//!   * `SystemParametersInfoW` — скорость курсора, колёс, кнопши;
//!   * SetupAPI/HID — перечисление устройств, чтение HID-репортов
//!     (диагностика и поиск протокола DPI).

#![allow(non_snake_case, non_camel_case_types, non_upper_case_globals, dead_code)]

use std::ffi::c_void;

pub type HANDLE = isize;
pub type HMODULE = isize;
pub type HWND = isize;
pub type HHOOK = isize;
pub type HINSTANCE = isize;
pub type BOOL = i32;
pub type DWORD = u32;
pub type UINT = u32;
pub type WORD = u16;
pub type LONG = i32;
pub type ULONG_PTR = usize;
pub type LRESULT = isize;
pub type LPARAM = isize;
pub type WPARAM = usize;
pub type LPCWSTR = *const u16;
pub type LPWSTR = *mut u16;

pub const INVALID_HANDLE_VALUE: HANDLE = -1;
pub const WH_MOUSE_LL: i32 = 14;
pub const WH_KEYBOARD_LL: i32 = 13;
pub const WM_QUIT: u32 = 0x0012;
pub const PM_REMOVE: u32 = 0x0001;

// ---------------------------------------------------------------- хуки
pub const HC_ACTION: i32 = 0;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct POINT {
    pub x: i32,
    pub y: i32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct MSLLHOOKSTRUCT {
    pub pt: POINT,
    pub mouseData: DWORD,
    pub flags: DWORD,
    pub time: DWORD,
    pub dwExtraInfo: ULONG_PTR,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct KBDLLHOOKSTRUCT {
    pub vkCode: DWORD,
    pub scanCode: DWORD,
    pub flags: DWORD,
    pub time: DWORD,
    pub dwExtraInfo: ULONG_PTR,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct MSG {
    pub hwnd: HWND,
    pub message: UINT,
    pub wParam: WPARAM,
    pub lParam: LPARAM,
    pub time: DWORD,
    pub pt: POINT,
    pub lPrivate: DWORD,
}

pub type LowLevelProc = unsafe extern "system" fn(i32, WPARAM, LPARAM) -> LRESULT;

// ---------------------------------------------------------------- ввод
pub const INPUT_MOUSE: u32 = 0;
pub const INPUT_KEYBOARD: u32 = 1;

pub const MOUSEEVENTF_MOVE: u32 = 0x0001;
pub const MOUSEEVENTF_LEFTDOWN: u32 = 0x0002;
pub const MOUSEEVENTF_LEFTUP: u32 = 0x0004;
pub const MOUSEEVENTF_RIGHTDOWN: u32 = 0x0008;
pub const MOUSEEVENTF_RIGHTUP: u32 = 0x0010;
pub const MOUSEEVENTF_MIDDLEDOWN: u32 = 0x0020;
pub const MOUSEEVENTF_MIDDLEUP: u32 = 0x0040;
pub const MOUSEEVENTF_WHEEL: u32 = 0x0800;
pub const MOUSEEVENTF_HWHEEL: u32 = 0x1000;
pub const MOUSEEVENTF_XDOWN: u32 = 0x0080;
pub const MOUSEEVENTF_XUP: u32 = 0x0100;

pub const KEYEVENTF_KEYUP: u32 = 0x0002;
pub const KEYEVENTF_EXTENDEDKEY: u32 = 0x0001;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct MOUSEINPUT {
    pub dx: LONG,
    pub dy: LONG,
    pub mouseData: DWORD,
    pub dwFlags: DWORD,
    pub time: DWORD,
    pub dwExtraInfo: ULONG_PTR,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct KEYBDINPUT {
    pub wVk: WORD,
    pub wScan: WORD,
    pub dwFlags: DWORD,
    pub time: DWORD,
    pub dwExtraInfo: ULONG_PTR,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub union INPUT_UNION {
    pub mi: MOUSEINPUT,
    pub ki: KEYBDINPUT,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct INPUT {
    pub type_: u32,
    pub u: INPUT_UNION,
}

impl INPUT {
    pub fn mouse(flags: u32, data: i32) -> Self {
        INPUT {
            type_: INPUT_MOUSE,
            u: INPUT_UNION {
                mi: MOUSEINPUT {
                    dx: 0,
                    dy: 0,
                    mouseData: data as u32,
                    dwFlags: flags,
                    time: 0,
                    dwExtraInfo: G_MOUSE_MARK,
                },
            },
        }
    }

    pub fn key(vk: u16, up: bool) -> Self {
        INPUT {
            type_: INPUT_KEYBOARD,
            u: INPUT_UNION {
                ki: KEYBDINPUT {
                    wVk: vk,
                    wScan: 0,
                    dwFlags: if up { KEYEVENTF_KEYUP } else { 0 },
                    time: 0,
                    dwExtraInfo: G_MOUSE_MARK,
                },
            },
        }
    }
}

/// Метка на синтезированных событиях: хук видит её и пропускает их дальше,
/// иначе переназначение «кнопка → та же кнопка» ушло бы в бесконечный цикл.
pub const G_MOUSE_MARK: usize = 0x0047_4D_4F_55;

// ---------------------------------------------------------------- параметры
pub const SPI_GETMOUSESPEED: u32 = 0x0070;
pub const SPI_SETMOUSESPEED: u32 = 0x0071;
pub const SPI_GETWHEELSCROLLLINES: u32 = 0x0068;
pub const SPI_SETWHEELSCROLLLINES: u32 = 0x0069;
pub const SPI_GETWHEELSCROLLCHARS: u32 = 0x006C;
pub const SPI_SETWHEELSCROLLCHARS: u32 = 0x006D;
pub const SPI_GETMOUSE: u32 = 0x0003;
pub const SPI_SETMOUSE: u32 = 0x0004;
pub const SPI_GETMOUSEHOVERTIME: u32 = 0x0066;
pub const SPI_SETMOUSEHOVERTIME: u32 = 0x0067;
pub const SPI_GETMOUSEHOVERWIDTH: u32 = 0x0062;
pub const SPI_SETMOUSEHOVERWIDTH: u32 = 0x0063;
pub const SPI_GETMOUSEHOVERHEIGHT: u32 = 0x0064;
pub const SPI_SETMOUSEHOVERHEIGHT: u32 = 0x0065;
pub const SPIF_UPDATEINIFILE: u32 = 0x01;
pub const SPIF_SENDCHANGE: u32 = 0x02;

pub const SM_SWAPBUTTON: i32 = 23;
pub const SM_CXDOUBLECLK: i32 = 36;
pub const SM_CYDOUBLECLK: i32 = 37;

// ---------------------------------------------------------------- HID
#[repr(C)]
#[derive(Clone, Copy)]
pub struct GUID {
    pub Data1: u32,
    pub Data2: u16,
    pub Data3: u16,
    pub Data4: [u8; 8],
}

pub const GUID_DEVINTERFACE_HID: GUID = GUID {
    Data1: 0x4D1E55B2,
    Data2: 0xF16F,
    Data3: 0x11CF,
    Data4: [0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30],
};

pub const DIGCF_PRESENT: u32 = 0x02;
pub const DIGCF_DEVICEINTERFACE: u32 = 0x10;
pub const FILE_SHARE_READ: u32 = 0x01;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SP_DEVICE_INTERFACE_DATA {
    pub cbSize: u32,
    pub InterfaceClassGuid: GUID,
    pub Flags: u32,
    pub Reserved: ULONG_PTR,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct HIDD_ATTRIBUTES {
    pub Size: u32,
    pub VendorID: u16,
    pub ProductID: u16,
    pub VersionNumber: u16,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct HIDP_CAPS {
    pub Usage: u16,
    pub UsagePage: u16,
    pub InputReportByteLength: u16,
    pub OutputReportByteLength: u16,
    pub FeatureReportByteLength: u16,
    pub Reserved: [u16; 17],
    pub NumberLinkCollectionNodes: u16,
    pub NumberInputButtonCaps: u16,
    pub NumberInputValueCaps: u16,
    pub NumberInputDataIndices: u16,
    pub NumberOutputButtonCaps: u16,
    pub NumberOutputValueCaps: u16,
    pub NumberOutputDataIndices: u16,
    pub NumberFeatureButtonCaps: u16,
    pub NumberFeatureValueCaps: u16,
    pub NumberFeatureDataIndices: u16,
}

pub const HIDP_STATUS_SUCCESS: i32 = 0x0011_0000;
pub const HIDP_INPUT: i32 = 0;
pub const HIDP_OUTPUT: i32 = 1;
pub const HIDP_FEATURE: i32 = 2;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct HIDP_VALUE_CAPS {
    pub UsagePage: u16,
    pub ReportID: u8,
    pub IsAlias: u8,
    pub BitField: u16,
    pub LinkCollection: u16,
    pub LinkUsage: u16,
    pub LinkUsagePage: u16,
    pub IsRange: u8,
    pub IsStringRange: u8,
    pub IsDesignatorRange: u8,
    pub IsAbsolute: u8,
    pub HasNull: u8,
    pub Reserved: u8,
    pub BitSize: u16,
    pub ReportCount: u16,
    pub Reserved2: [u16; 5],
    pub UnitsExp: u32,
    pub Units: u32,
    pub LogicalMin: i32,
    pub LogicalMax: i32,
    pub PhysicalMin: i32,
    pub PhysicalMax: i32,
    pub UsageMin: u16,
    pub UsageMax: u16,
    pub StringMin: u16,
    pub StringMax: u16,
    pub DesignatorMin: u16,
    pub DesignatorMax: u16,
    pub DataIndexMin: u16,
    pub DataIndexMax: u16,
}

#[link(name = "user32")]
extern "system" {
    pub fn SetWindowsHookExW(idHook: i32, lpfn: Option<LowLevelProc>,
                             hmod: HMODULE, dwThreadId: DWORD) -> HHOOK;
    pub fn UnhookWindowsHookEx(hhk: HHOOK) -> BOOL;
    pub fn CallNextHookEx(hhk: HHOOK, nCode: i32, wParam: WPARAM,
                          lParam: LPARAM) -> LRESULT;
    pub fn GetMessageW(msg: *mut MSG, hwnd: HWND, min: UINT, max: UINT) -> BOOL;
    pub fn PeekMessageW(msg: *mut MSG, hwnd: HWND, min: UINT, max: UINT,
                        remove: UINT) -> BOOL;
    pub fn PostThreadMessageW(thread_id: DWORD, msg: UINT, w: WPARAM,
                              l: LPARAM) -> BOOL;
    pub fn GetCurrentThreadId() -> DWORD;
    pub fn SendInput(count: UINT, inputs: *const INPUT, size: i32) -> UINT;
    pub fn SystemParametersInfoW(action: u32, param: u32, pv: *mut c_void,
                                 win_ini: u32) -> BOOL;
    pub fn GetSystemMetrics(index: i32) -> i32;
    pub fn GetLastError() -> u32;
}

#[link(name = "kernel32")]
extern "system" {
    pub fn GetModuleHandleW(name: LPCWSTR) -> HMODULE;
    pub fn CreateFileW(name: LPCWSTR, access: DWORD, share: DWORD,
                       sec: *mut c_void, disposition: DWORD, flags: DWORD,
                       template: HANDLE) -> HANDLE;
    pub fn CloseHandle(h: HANDLE) -> BOOL;
    pub fn ReadFile(h: HANDLE, buf: *mut c_void, to_read: DWORD,
                    read: *mut DWORD, overlapped: *mut c_void) -> BOOL;
    pub fn WriteFile(h: HANDLE, buf: *const c_void, to_write: DWORD,
                     written: *mut DWORD, overlapped: *mut c_void) -> BOOL;
    pub fn GetOverlappedResult(h: HANDLE, ov: *mut c_void,
                               transferred: *mut DWORD, wait: BOOL) -> BOOL;
    pub fn DeviceIoControl(h: HANDLE, code: DWORD, in_buf: *mut c_void,
                           in_size: DWORD, out_buf: *mut c_void,
                           out_size: DWORD, returned: *mut DWORD,
                           overlapped: *mut c_void) -> BOOL;
}

#[link(name = "hid")]
extern "system" {
    pub fn HidD_GetHidGuid(guid: *mut GUID);
    pub fn HidD_GetAttributes(h: HANDLE, attrs: *mut HIDD_ATTRIBUTES) -> BOOL;
    pub fn HidD_GetProductString(h: HANDLE, buf: *mut c_void, len: u32) -> BOOL;
    pub fn HidD_GetManufacturerString(h: HANDLE, buf: *mut c_void,
                                      len: u32) -> BOOL;
    pub fn HidD_GetSerialNumberString(h: HANDLE, buf: *mut c_void,
                                      len: u32) -> BOOL;
    pub fn HidD_GetPreparsedData(h: HANDLE, pp: *mut *mut c_void) -> BOOL;
    pub fn HidD_FreePreparsedData(pp: *mut c_void) -> BOOL;
    pub fn HidD_GetFeature(h: HANDLE, buf: *mut c_void, len: u32) -> BOOL;
    pub fn HidD_GetInputReport(h: HANDLE, buf: *mut c_void, len: u32) -> BOOL;
    pub fn HidD_SetFeature(h: HANDLE, buf: *const c_void, len: u32) -> BOOL;
    pub fn HidD_SetOutputReport(h: HANDLE, buf: *const c_void, len: u32) -> BOOL;
    pub fn HidP_GetCaps(pp: *mut c_void, caps: *mut HIDP_CAPS) -> i32;
    pub fn HidP_GetValueCaps(report_type: i32, caps: *mut HIDP_VALUE_CAPS,
                             len: *mut u16, pp: *mut c_void) -> i32;
    pub fn HidP_GetUsageValue(report_type: i32, usage_page: u16,
                              link_collection: u16, usage: u16,
                              value: *mut u32, pp: *mut c_void,
                              report: *const u8, len: u32) -> i32;
    pub fn HidP_GetScaledUsageValue(report_type: i32, usage_page: u16,
                                    link_collection: u16, usage: u16,
                                    value: *mut i32, pp: *mut c_void,
                                    report: *const u8, len: u32) -> i32;
}

#[link(name = "setupapi")]
extern "system" {
    pub fn SetupDiGetClassDevsW(guid: *const GUID, enumerator: LPCWSTR,
                                parent: HWND, flags: DWORD) -> HANDLE;
    pub fn SetupDiEnumDeviceInterfaces(devs: HANDLE, info: *mut c_void,
                                       guid: *const GUID, index: DWORD,
                                       data: *mut SP_DEVICE_INTERFACE_DATA)
        -> BOOL;
    pub fn SetupDiGetDeviceInterfaceDetailW(
        devs: HANDLE, data: *const SP_DEVICE_INTERFACE_DATA, detail: *mut c_void,
        detail_size: DWORD, required: *mut DWORD,
        dev_info: *mut c_void) -> BOOL;
    pub fn SetupDiDestroyDeviceInfoList(devs: HANDLE) -> BOOL;
}
