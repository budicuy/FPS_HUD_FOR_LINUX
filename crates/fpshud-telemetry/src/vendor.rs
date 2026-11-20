//! Optional runtime vendor GPU telemetry (`libnvidia-ml.so.1` / `libze_loader.so.1`).

#[must_use]
pub fn probe_optional_vendor_library(soname: &str) -> bool {
    unsafe { libloading::Library::new(soname).is_ok() }
}
