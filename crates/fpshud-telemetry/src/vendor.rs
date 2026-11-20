//! Optional runtime vendor GPU telemetry (`libnvidia-ml.so.1` / `libze_loader.so.1`), zero `DT_NEEDED`, zero `#[repr(C)]`.

use crate::snapshot::{GpuBackendType, TelemetrySnapshot};
use libloading::Library;
use std::ffi::{CStr, c_char, c_int, c_uint, c_void};
use std::fs;
use std::path::Path;

const NVML_SUCCESS: c_int = 0;
const NVML_TEMPERATURE_GPU: c_uint = 0;
const NVML_CLOCK_GRAPHICS: c_uint = 0;
const NVML_CLOCK_MEM: c_uint = 2;

#[must_use]
pub fn probe_optional_vendor_library(soname: &str) -> bool {
    unsafe { Library::new(soname).is_ok() }
}

/// Checks sysfs `vendor` & `power/runtime_status` to avoid waking a D3cold-suspended NVIDIA GPU.
#[must_use]
pub fn is_nvidia_d3cold_suspended(root: &Path) -> bool {
    let mut has_nvidia = false;
    let mut all_suspended = true;

    for rel in ["sys/bus/pci/devices", "sys/class/drm"] {
        let Ok(entries) = fs::read_dir(root.join(rel)) else {
            continue;
        };
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().into_owned();
            let dev_dir = if rel == "sys/class/drm" {
                if !name.starts_with("card") || name.contains('-') {
                    continue;
                }
                entry.path().join("device")
            } else {
                entry.path()
            };
            let Ok(vendor_raw) = fs::read_to_string(dev_dir.join("vendor")) else {
                continue;
            };
            let stripped = vendor_raw.trim().strip_prefix("0x").unwrap_or(vendor_raw.trim());
            let Ok(vendor_id) = u32::from_str_radix(stripped, 16) else {
                continue;
            };
            if vendor_id != 0x10de {
                continue;
            }
            has_nvidia = true;
            let status = fs::read_to_string(dev_dir.join("power/runtime_status"))
                .unwrap_or_default()
                .trim()
                .to_ascii_lowercase();
            if !status.contains("suspended") {
                all_suspended = false;
            }
        }
    }

    has_nvidia && all_suspended
}

#[must_use]
pub fn compute_nvml_bandwidth_gbs(mem_clock_mhz: u32, bus_width_bits: u32, mem_util_pct: u32) -> f32 {
    let max_bw_gbs =
        (mem_clock_mhz as f32) * 2.0 * ((bus_width_bits as f32) / 8.0) / 1000.0;
    max_bw_gbs * ((mem_util_pct as f32) / 100.0)
}

#[derive(Clone, Copy)]
struct NvmlDevice(*mut c_void);
unsafe impl Send for NvmlDevice {}

pub struct NvidiaNvmlProvider {
    lib: Library,
    device: NvmlDevice,
    gpu_name: String,
    bus_width_bits: u32,
    fn_shutdown: unsafe extern "C" fn() -> c_int,
    fn_util: Option<unsafe extern "C" fn(*mut c_void, *mut u32) -> c_int>,
    fn_mem: Option<unsafe extern "C" fn(*mut c_void, *mut u64) -> c_int>,
    fn_temp: Option<unsafe extern "C" fn(*mut c_void, c_uint, *mut c_uint) -> c_int>,
    fn_fan: Option<unsafe extern "C" fn(*mut c_void, *mut c_uint) -> c_int>,
    fn_power: Option<unsafe extern "C" fn(*mut c_void, *mut c_uint) -> c_int>,
    fn_clock: Option<unsafe extern "C" fn(*mut c_void, c_uint, *mut c_uint) -> c_int>,
}

impl NvidiaNvmlProvider {
    pub fn init_for_root(root: &Path, vendor_id: u32, device_id: u32) -> Option<Self> {
        if root != Path::new("/") {
            return None;
        }
        if vendor_id != 0 && vendor_id != 0x10de {
            return None;
        }
        if is_nvidia_d3cold_suspended(root) && vendor_id != 0x10de {
            return None;
        }

        const CANDIDATES: [&str; 4] = [
            "libnvidia-ml.so.1",
            "libnvidia-ml.so",
            "/run/host/usr/lib/x86_64-linux-gnu/libnvidia-ml.so.1",
            "/run/host/usr/lib64/libnvidia-ml.so.1",
        ];
        let lib = CANDIDATES
            .iter()
            .find_map(|path| unsafe { Library::new(path).ok() })?;

        let fn_init: unsafe extern "C" fn() -> c_int =
            unsafe { *lib.get::<unsafe extern "C" fn() -> c_int>(b"nvmlInit_v2").ok()? };
        let fn_shutdown: unsafe extern "C" fn() -> c_int =
            unsafe { *lib.get::<unsafe extern "C" fn() -> c_int>(b"nvmlShutdown").ok()? };
        let fn_handle: unsafe extern "C" fn(c_uint, *mut *mut c_void) -> c_int = unsafe {
            *lib.get::<unsafe extern "C" fn(c_uint, *mut *mut c_void) -> c_int>(
                b"nvmlDeviceGetHandleByIndex_v2",
            )
            .ok()?
        };
        let fn_count = unsafe {
            lib.get::<unsafe extern "C" fn(*mut c_uint) -> c_int>(b"nvmlDeviceGetCount_v2")
                .ok()
                .map(|s| *s)
        };
        let fn_name = unsafe {
            lib.get::<unsafe extern "C" fn(*mut c_void, *mut c_char, c_uint) -> c_int>(
                b"nvmlDeviceGetName",
            )
            .ok()
            .map(|s| *s)
        };
        let fn_util = unsafe {
            lib.get::<unsafe extern "C" fn(*mut c_void, *mut u32) -> c_int>(
                b"nvmlDeviceGetUtilizationRates",
            )
            .ok()
            .map(|s| *s)
        };
        let fn_mem = unsafe {
            lib.get::<unsafe extern "C" fn(*mut c_void, *mut u64) -> c_int>(
                b"nvmlDeviceGetMemoryInfo",
            )
            .ok()
            .map(|s| *s)
        };
        let fn_temp = unsafe {
            lib.get::<unsafe extern "C" fn(*mut c_void, c_uint, *mut c_uint) -> c_int>(
                b"nvmlDeviceGetTemperature",
            )
            .ok()
            .map(|s| *s)
        };
        let fn_fan = unsafe {
            lib.get::<unsafe extern "C" fn(*mut c_void, *mut c_uint) -> c_int>(
                b"nvmlDeviceGetFanSpeed",
            )
            .ok()
            .map(|s| *s)
        };
        let fn_power = unsafe {
            lib.get::<unsafe extern "C" fn(*mut c_void, *mut c_uint) -> c_int>(
                b"nvmlDeviceGetPowerUsage",
            )
            .ok()
            .map(|s| *s)
        };
        let fn_clock = unsafe {
            lib.get::<unsafe extern "C" fn(*mut c_void, c_uint, *mut c_uint) -> c_int>(
                b"nvmlDeviceGetClockInfo",
            )
            .ok()
            .map(|s| *s)
        };
        let fn_bus_width = unsafe {
            lib.get::<unsafe extern "C" fn(*mut c_void, *mut c_uint) -> c_int>(
                b"nvmlDeviceGetMemoryBusWidth",
            )
            .ok()
            .map(|s| *s)
        };
        let fn_pci = unsafe {
            lib.get::<unsafe extern "C" fn(*mut c_void, *mut u32) -> c_int>(
                b"nvmlDeviceGetPciInfo_v3",
            )
            .or_else(|_| {
                lib.get::<unsafe extern "C" fn(*mut c_void, *mut u32) -> c_int>(
                    b"nvmlDeviceGetPciInfo",
                )
            })
            .ok()
            .map(|s| *s)
        };

        if unsafe { fn_init() } != NVML_SUCCESS {
            return None;
        }

        let mut dev_count = 1_u32;
        if let Some(get_cnt) = fn_count {
            let mut cnt = 0_u32;
            if unsafe { get_cnt(&mut cnt) } == NVML_SUCCESS && cnt > 0 {
                dev_count = cnt.min(8);
            }
        }

        let mut best_handle: *mut c_void = std::ptr::null_mut();
        let mut best_vram = 0_u64;
        for idx in 0..dev_count {
            let mut h: *mut c_void = std::ptr::null_mut();
            if unsafe { fn_handle(idx, &mut h) } != NVML_SUCCESS || h.is_null() {
                continue;
            }
            if device_id != 0
                && let Some(get_pci) = fn_pci
            {
                let mut pci_words = [0_u32; 17];
                if unsafe { get_pci(h, pci_words.as_mut_ptr()) } == NVML_SUCCESS {
                    // Word 7 (bytes 28..32) = pciDeviceId ((device_id << 16) | vendor_id) in nvmlPciInfo_t
                    let pci_dev_id = pci_words[7];
                    if (pci_dev_id >> 16) == device_id || (pci_dev_id & 0xffff) == device_id {
                        best_handle = h;
                        break;
                    }
                }
            }
            let mut total_vram = 0_u64;
            if let Some(get_mem) = fn_mem {
                let mut mem = [0_u64; 3];
                if unsafe { get_mem(h, mem.as_mut_ptr()) } == NVML_SUCCESS {
                    total_vram = mem[0];
                }
            }
            if best_handle.is_null() || total_vram > best_vram {
                best_handle = h;
                best_vram = total_vram;
            }
        }

        if best_handle.is_null() {
            unsafe {
                let _ = fn_shutdown();
            }
            return None;
        }

        let mut gpu_name = "NVIDIA GPU".to_string();
        if let Some(get_name) = fn_name {
            let mut buf = [0_u8; 96];
            if unsafe { get_name(best_handle, buf.as_mut_ptr().cast::<c_char>(), buf.len() as c_uint) }
                == NVML_SUCCESS
                && let Ok(cstr) = CStr::from_bytes_until_nul(&buf)
            {
                let raw = cstr.to_string_lossy().trim().to_string();
                if !raw.is_empty() {
                    gpu_name = raw;
                }
            }
        }

        let mut bus_width_bits = 0_u32;
        if let Some(get_bw) = fn_bus_width {
            let mut bw = 0_u32;
            if unsafe { get_bw(best_handle, &mut bw) } == NVML_SUCCESS {
                bus_width_bits = bw;
            }
        }

        Some(Self {
            lib,
            device: NvmlDevice(best_handle),
            gpu_name,
            bus_width_bits,
            fn_shutdown,
            fn_util,
            fn_mem,
            fn_temp,
            fn_fan,
            fn_power,
            fn_clock,
        })
    }

    pub fn poll(&mut self, snap: &mut TelemetrySnapshot) -> bool {
        let dev = self.device.0;
        if dev.is_null() {
            return false;
        }
        let _keep_lib_alive = &self.lib;

        snap.gpu_available = true;
        snap.gpu_backend = GpuBackendType::Nvml;
        snap.gpu_model = self.gpu_name.clone();

        let mut mem_util_pct = 0_u32;
        let mut ok_util = false;
        if let Some(get_util) = self.fn_util {
            let mut util = [0_u32; 2];
            if unsafe { get_util(dev, util.as_mut_ptr()) } == NVML_SUCCESS {
                snap.gpu_load = (util[0] as f32).clamp(0.0, 100.0);
                mem_util_pct = util[1].min(100);
                ok_util = true;
            }
        }

        if let Some(get_mem) = self.fn_mem {
            let mut mem = [0_u64; 3];
            if unsafe { get_mem(dev, mem.as_mut_ptr()) } == NVML_SUCCESS {
                snap.gpu_vram_total_gb = (mem[0] as f32) / (1024.0 * 1024.0 * 1024.0);
                snap.gpu_vram_used_gb = (mem[2] as f32) / (1024.0 * 1024.0 * 1024.0);
            }
        }

        if let Some(get_clk) = self.fn_clock {
            let mut gr_mhz = 0_u32;
            if unsafe { get_clk(dev, NVML_CLOCK_GRAPHICS, &mut gr_mhz) } == NVML_SUCCESS {
                snap.gpu_clock_mhz = gr_mhz;
            }
            let mut mem_mhz = 0_u32;
            if unsafe { get_clk(dev, NVML_CLOCK_MEM, &mut mem_mhz) } == NVML_SUCCESS {
                snap.gpu_vram_clock_mhz = mem_mhz;
                if self.bus_width_bits > 0 && ok_util {
                    snap.gpu_bandwidth_gbs =
                        compute_nvml_bandwidth_gbs(mem_mhz, self.bus_width_bits, mem_util_pct);
                }
            }
        }

        if let Some(get_temp) = self.fn_temp {
            let mut temp = 0_u32;
            if unsafe { get_temp(dev, NVML_TEMPERATURE_GPU, &mut temp) } == NVML_SUCCESS {
                snap.gpu_temp_c = temp as f32;
            }
        }

        if let Some(get_fan) = self.fn_fan {
            let mut fan = 0_u32;
            if unsafe { get_fan(dev, &mut fan) } == NVML_SUCCESS {
                snap.gpu_fan_pct = fan.min(100);
            }
        }

        if let Some(get_pwr) = self.fn_power {
            let mut mw = 0_u32;
            if unsafe { get_pwr(dev, &mut mw) } == NVML_SUCCESS {
                snap.gpu_power_w = (mw as f32) / 1000.0;
            }
        }

        true
    }
}

impl Drop for NvidiaNvmlProvider {
    fn drop(&mut self) {
        unsafe {
            let _ = (self.fn_shutdown)();
        }
    }
}
